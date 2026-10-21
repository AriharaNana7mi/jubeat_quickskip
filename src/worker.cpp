// ---------------------------------------------------------------------------
//  worker.cpp - background thread of the injected DLL.
//
//  Two build flavours:
//    * generic hook_dll.dll : module map, wait_for_module("jubeat.dll"),
//      pattern-scan proof, demo inline/IAT hooks, hotkey polling.
//    * jubeat_probe.dll     (-DHARNESS_PROBE): module base logging only, the
//      probe feature (feature_probe.cpp) owns all game interaction; nothing
//      is hooked until the probe has verified modules + jubeat version.
// ---------------------------------------------------------------------------
#include "common/common.h"
#include "feature.h"
#include "worker.h"

#ifdef HARNESS_PROBE
#include "probe/probe_api.h"
#else
#include "demo_hooks.h"
#endif

#include <cstdlib>

namespace harness {
namespace worker {
namespace {

HANDLE        g_thread = nullptr;
volatile LONG g_stop = 0;

#ifndef HARNESS_PROBE
std::string GetEnvA(const char* name) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return std::string();
    return std::string(buf, n);
}

int ParseHotkeys(const std::string& spec, int* keys, int maxKeys) {
    int count = 0;
    const char* p = spec.c_str();
    while (*p && count < maxKeys) {
        while (*p == ',' || *p == ' ' || *p == ';') ++p;
        if (!*p) break;
        char* end = nullptr;
        long v = strtol(p, &end, 0);
        if (end == p) break;
        keys[count++] = (int)v;
        p = end;
    }
    return count;
}
#endif

DWORD WINAPI ThreadProc(LPVOID) {
    Logf("worker: started tid=%lu", GetCurrentThreadId());

    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH * 2);
    HMODULE exeModule = GetModuleHandleW(nullptr);
    uintptr_t exeBase = 0;
    size_t exeSize = 0;
    GetModuleRange(exeModule, &exeBase, &exeSize);
    Logf("worker: pid=%lu exe=%ls", GetCurrentProcessId(), exePath);
    Logf("worker: main module base=%p size=0x%zX log=%ls",
         (void*)exeBase, exeSize, LogPath());

    // ---- module map (proof: module bases visible from inside the process) --
    std::vector<ModuleInfo> mods = EnumerateModules(GetCurrentProcessId());
    Logf("worker: module count=%zu", mods.size());
    for (size_t i = 0; i < mods.size(); ++i) {
        Logf("  mod[%02zu] base=%p size=0x%08lX %ls", i,
             (void*)mods[i].base, (unsigned long)mods[i].size,
             mods[i].name.c_str());
    }

#ifdef HARNESS_PROBE
    // ---- probe flavour: feature owns module waiting / hooking / dumps ----
    feature::OnDllReady();
    Logf("worker: probe feature initialised, entering tick loop");
    DWORD startProbe = GetTickCount();
    while (!StopRequested()) {
        feature::OnTick(GetTickCount() - startProbe);
        for (int i = 0; i < 2 && !StopRequested(); ++i) Sleep(20);
    }
    Logf("worker: stop requested -> teardown (probe)");
    feature::OnShutdown();
    HookRemoveAll();
    IatUnhookAll();
    Logf("worker: exit");
    return 0;
#else
    // ---- generic hook_dll.dll flavour ------------------------------------
    DWORD waited = 0;
    HMODULE jb = WaitForModule("jubeat.dll", 2000, 25, &waited);
    Logf("wait_for_module: jubeat.dll -> base=%p waited=%lu ms %s", (void*)jb,
         (unsigned long)waited,
         jb ? "FOUND" : "not loaded (expected for dummy target)");
    if (jb) {
        uintptr_t b = 0;
        size_t sz = 0;
        GetModuleRange(jb, &b, &sz);
        Logf("jubeat.dll: base=%p size=0x%zX preferred=0x10000000 (ASLR may "
             "relocate; always use base+offset)",
             (void*)b, sz);
        std::string scanSpec = GetEnvA("JUBEAT_SCAN_PATTERN");
        if (!scanSpec.empty()) {
            uintptr_t hit = ScanModule(jb, scanSpec.c_str());
            Logf("pattern scan jubeat.dll [%s] -> %p offset=0x%zX %s",
                 scanSpec.c_str(), (void*)hit, hit ? hit - b : 0,
                 hit ? "FOUND" : "not found");
        }
    }

    std::string waitMod = GetEnvA("HOOK_WAIT_MODULE");
    if (!waitMod.empty()) {
        DWORD w = 0;
        HMODULE m = WaitForModule(waitMod.c_str(), 20000, 25, &w);
        Logf("wait_for_module: %s -> base=%p waited=%lu ms %s",
             waitMod.c_str(), (void*)m, (unsigned long)w,
             m ? "FOUND" : "TIMEOUT");
    }

    {
        const char* marker = "DE AD BE EF 48 41 52 4E 45 53 53 00 12 34 56 78";
        uintptr_t hit = ScanModuleAny(exeModule, marker);
        Logf("pattern scan main module marker [%s] -> %p offset=0x%zX %s",
             marker, (void*)hit, hit ? hit - exeBase : 0,
             hit ? "FOUND (all sections)" : "not found");
    }

    int keys[kMaxHotkeys] = {0x31, 0x32, 0x33, 0x34};
    int keyCount = 4;
    std::string hkSpec = GetEnvA("HOOK_HOTKEYS");
    if (!hkSpec.empty()) {
        int n = ParseHotkeys(hkSpec, keys, kMaxHotkeys);
        if (n > 0) keyCount = n;
    }
    HotkeyConfigure(keys, keyCount);
    Logf("hotkey poller: %d keys configured%s", keyCount,
         hkSpec.empty() ? "" : " (HOOK_HOTKEYS)");

    feature::OnDllReady();
    demo::Install();

    DWORD start = GetTickCount();
    while (!StopRequested()) {
        if (HotkeyAllHeldRisingEdge()) {
            Logf("hotkey: all %d keys held (rising edge) -> feature::OnHotkey",
                 keyCount);
            feature::OnHotkey();
        }
        feature::OnTick(GetTickCount() - start);
        for (int i = 0; i < 2 && !StopRequested(); ++i) Sleep(20);
    }

    Logf("worker: stop requested -> teardown");
    demo::Remove();
    HookRemoveAll();
    IatUnhookAll();
    feature::OnShutdown();
    Logf("worker: exit");
    return 0;
#endif
}

} // namespace

bool Start(HMODULE /*self*/) {
    if (g_thread) return true;
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void RequestStop() { InterlockedExchange(&g_stop, 1); }
bool StopRequested() { return g_stop != 0; }

bool Join(DWORD timeoutMs) {
    if (!g_thread) return true;
    DWORD r = WaitForSingleObject(g_thread, timeoutMs);
    return r == WAIT_OBJECT_0;
}

} // namespace worker
} // namespace harness
