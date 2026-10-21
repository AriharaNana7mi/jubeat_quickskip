// ---------------------------------------------------------------------------
//  dllmain.cpp - DllMain + exported test entry points of hook_dll.dll.
//
//  Design rules
//  ------------
//  * DllMain only does cheap work (init logger, start worker thread).
//  * All module waiting / scanning / hooking happens on the worker thread,
//    never under the loader lock.
//  * Every exported entry point takes exactly one DWORD argument so it can be
//    invoked safely through CreateRemoteThread (BaseThreadInitThunk calls the
//    start routine with one parameter - a stdcall(void) export would corrupt
//    the stack).
//  * Teardown: worker removes hooks, unload path restores everything and then
//    FreeLibraryAndExitThread; DllMain detach is a best effort fallback and
//    never frees memory another thread may still be executing.
// ---------------------------------------------------------------------------
#include "common/common.h"
#include "feature.h"
#include "worker.h"

namespace {

HINSTANCE     g_self = nullptr;
volatile LONG g_cleanUnload = 0;
const DWORD   kHarnessVersion = 0x00010000; // 1.00

} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI HarnessVersion(DWORD) {
    return kHarnessVersion;
}

extern "C" __declspec(dllexport) DWORD WINAPI HarnessPing(DWORD arg) {
    harness::Logf("export: HarnessPing(arg=%lu) called inside pid=%lu tid=%lu",
                  (unsigned long)arg, (unsigned long)GetCurrentProcessId(),
                  (unsigned long)GetCurrentThreadId());
    return GetCurrentProcessId();
}

extern "C" __declspec(dllexport) DWORD WINAPI HarnessHotkeyTest(DWORD arg) {
    harness::Logf("export: HarnessHotkeyTest(arg=%lu) -> simulating the hotkey "
                  "callback",
                  (unsigned long)arg);
    feature::OnHotkey();
    return 1;
}

// Stops the worker, restores every hook, then unloads the DLL from inside the
// process.  Call through injector.exe --call HarnessUnload.
extern "C" __declspec(dllexport) DWORD WINAPI HarnessUnload(DWORD arg) {
    harness::Logf("export: HarnessUnload(arg=%lu) requested", (unsigned long)arg);
    harness::worker::RequestStop();
    harness::worker::Join(8000);
    harness::HookRemoveAll();
    harness::IatUnhookAll();
    // feature::OnShutdown() was already called by the worker thread's teardown.
    InterlockedExchange(&g_cleanUnload, 1);
    harness::Logf("export: hooks removed, calling FreeLibraryAndExitThread");
    harness::LogShutdown();
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hinst;
        DisableThreadLibraryCalls(hinst);
        harness::LogInit(hinst);
        harness::Logf("=== hook_dll.dll loaded (version 0x%08lX) pid=%lu base=%p",
                      (unsigned long)kHarnessVersion,
                      (unsigned long)GetCurrentProcessId(), (void*)hinst);
        if (!harness::worker::Start(hinst)) {
            harness::Logf("ERROR: worker thread could not be created, err=%lu",
                          (unsigned long)GetLastError());
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        harness::worker::RequestStop();
        if (InterlockedCompareExchange(&g_cleanUnload, 1, 1) == 0) {
            // Fallback detach (process exit or external FreeLibrary).
            // HookRemove suspends the other threads before restoring bytes and
            // trampolines are intentionally retired (not freed) so a thread
            // that is mid-trampoline can never execute freed memory.
            harness::HookRemoveAll();
            harness::IatUnhookAll();
            harness::Logf("DLL_PROCESS_DETACH: fallback teardown done");
        } else {
            harness::Logf("DLL_PROCESS_DETACH: clean unload path, already torn down");
        }
        harness::LogShutdown();
    }
    return TRUE;
}
