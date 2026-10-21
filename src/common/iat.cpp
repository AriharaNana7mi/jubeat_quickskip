// ---------------------------------------------------------------------------
//  iat.cpp - Import Address Table hook helper.
//
//  Patches one function pointer inside a module's import thunk table.  This
//  is the safest way to intercept a Win32 API for that specific module: no
//  code patching, no trampoline, and it does not disturb other modules (or
//  another tool's own copies of the API).
// ---------------------------------------------------------------------------
#include "common.h"

namespace harness {
namespace {

constexpr int kMaxIatHooks = 32;

struct IatEntry {
    void** slot = nullptr;
    void*  original = nullptr;
    void*  detour = nullptr;
    bool   active = false;
};

INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_cs;
IatEntry         g_entries[kMaxIatHooks];

BOOL CALLBACK InitOnceCb(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    return TRUE;
}
void EnsureCs() { InitOnceExecuteOnce(&g_once, InitOnceCb, nullptr, nullptr); }

bool NameEqualsI(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

bool ParseImports(HMODULE module, void*** outSlot, void** outOriginal,
                  const char* importDll, const char* funcName) {
    const uint8_t* base = (const uint8_t*)module;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress == 0)
        return false;

    const IMAGE_IMPORT_DESCRIPTOR* desc =
        (const IMAGE_IMPORT_DESCRIPTOR*)(base +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                .VirtualAddress);

    for (; desc->Name; ++desc) {
        const char* dllName = (const char*)(base + desc->Name);
        if (!NameEqualsI(dllName, importDll)) continue;

        DWORD oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                : desc->FirstThunk;
        const IMAGE_THUNK_DATA32* oft =
            (const IMAGE_THUNK_DATA32*)(base + oftRva);
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + desc->FirstThunk);

        for (size_t i = 0; oft[i].u1.AddressOfData; ++i) {
            if (oft[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const IMAGE_IMPORT_BY_NAME* byName =
                (const IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
            if (!NameEqualsI((const char*)byName->Name, funcName)) continue;
            *outSlot = (void**)&iat[i].u1.Function;
            *outOriginal = *(void**)&iat[i].u1.Function;
            return true;
        }
    }
    return false;
}

} // namespace

bool IatHook(HMODULE module, const char* importDll, const char* funcName,
             void* detour, void** original) {
    if (!module) module = GetModuleHandleA(nullptr);
    if (!module || !importDll || !funcName || !detour) return false;
    EnsureCs();
    EnterCriticalSection(&g_cs);

    IatEntry* slot = nullptr;
    for (auto& e : g_entries) {
        if (!e.active && !slot) slot = &e;
        if (e.active && e.detour == detour && e.active) {
            // do not double-hook the same detour
        }
    }
    if (!slot) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    void** iatSlot = nullptr;
    void*  orig = nullptr;
    if (!ParseImports(module, &iatSlot, &orig, importDll, funcName)) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(iatSlot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        LeaveCriticalSection(&g_cs);
        return false;
    }
    *iatSlot = detour;
    DWORD tmp = 0;
    VirtualProtect(iatSlot, sizeof(void*), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), iatSlot, sizeof(void*));

    slot->slot = iatSlot;
    slot->original = orig;
    slot->detour = detour;
    slot->active = true;
    if (original) *original = orig;
    LeaveCriticalSection(&g_cs);
    return true;
}

bool IatUnhook(HMODULE module, const char* importDll, const char* funcName,
               void* original) {
    EnsureCs();
    EnterCriticalSection(&g_cs);
    for (auto& e : g_entries) {
        if (!e.active) continue;
        if (original && e.original != original) continue;
        DWORD oldProt = 0;
        if (VirtualProtect(e.slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            *e.slot = e.original;
            DWORD tmp = 0;
            VirtualProtect(e.slot, sizeof(void*), oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), e.slot, sizeof(void*));
        }
        e.slot = nullptr;
        e.original = nullptr;
        e.detour = nullptr;
        e.active = false;
        LeaveCriticalSection(&g_cs);
        return true;
    }
    LeaveCriticalSection(&g_cs);
    return false;
}

void IatUnhookAll() {
    EnsureCs();
    void* originals[kMaxIatHooks];
    int count = 0;
    EnterCriticalSection(&g_cs);
    for (auto& e : g_entries)
        if (e.active) originals[count++] = e.original;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < count; ++i) IatUnhook(nullptr, nullptr, nullptr, originals[i]);
}

} // namespace harness
