// ---------------------------------------------------------------------------
//  modules.cpp - module enumeration / wait-for-module / PE range helpers.
// ---------------------------------------------------------------------------
#include "common.h"

namespace harness {

std::vector<ModuleInfo> EnumerateModules(DWORD pid) {
    std::vector<ModuleInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           pid);
    if (snap == INVALID_HANDLE_VALUE) return out;

    MODULEENTRY32W me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleInfo mi;
            mi.name = me.szModule;
            mi.path = me.szExePath;
            mi.base = (uintptr_t)me.modBaseAddr;
            mi.size = me.modBaseSize;
            out.push_back(std::move(mi));
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

bool GetModuleRange(HMODULE mod, uintptr_t* outBase, size_t* outSize) {
    if (!mod) return false;
    const uint8_t* p = (const uint8_t*)mod;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)p;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    if (outBase) *outBase = (uintptr_t)mod;
    if (outSize) *outSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

HMODULE WaitForModule(const char* name, DWORD timeoutMs, DWORD pollMs,
                      DWORD* waitedMs) {
    DWORD start = GetTickCount();
    HMODULE found = nullptr;
    for (;;) {
        found = GetModuleHandleA(name);
        if (found) break;
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) break;
        DWORD remain = timeoutMs - elapsed;
        Sleep(pollMs < remain ? pollMs : remain);
    }
    if (waitedMs) *waitedMs = GetTickCount() - start;
    return found;
}

} // namespace harness
