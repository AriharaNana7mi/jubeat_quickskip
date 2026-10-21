// ---------------------------------------------------------------------------
//  common.h - shared plumbing for the jubeat injection harness (32-bit only)
//
//  Used by hook_dll.dll.  Plain C++17, Win32 API only, no external deps.
// ---------------------------------------------------------------------------
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

// ---------------------------------------------------------------------------
//  Logging (mutex protected, append, never uses hooked APIs)
// ---------------------------------------------------------------------------
void  LogInit(HMODULE self);
void  LogShutdown();
const wchar_t* LogPath();
void  Logf(const char* fmt, ...);
void  LogLine(const char* text);

// ---------------------------------------------------------------------------
//  Modules
// ---------------------------------------------------------------------------
struct ModuleInfo {
    std::wstring name;
    std::wstring path;
    uintptr_t    base;
    DWORD        size;
};

std::vector<ModuleInfo> EnumerateModules(DWORD pid);
bool GetModuleRange(HMODULE mod, uintptr_t* outBase, size_t* outSize);
HMODULE WaitForModule(const char* name, DWORD timeoutMs, DWORD pollMs = 25,
                      DWORD* waitedMs = nullptr);

// ---------------------------------------------------------------------------
//  Pattern scanning (IDA style: "48 8B ?? ?? E8" / nibble wildcards "4? ??")
// ---------------------------------------------------------------------------
struct BytePattern {
    std::vector<uint8_t> value;
    std::vector<uint8_t> mask;
    size_t length = 0;
    bool   valid  = false;
};

bool      ParsePattern(const char* idaStyle, BytePattern* out);
uintptr_t ScanRange(uintptr_t start, size_t size, const BytePattern& pat);
uintptr_t ScanModule(HMODULE mod, const char* idaStyle, bool executableOnly = true);
uintptr_t ScanModuleAny(HMODULE mod, const char* idaStyle);

// ---------------------------------------------------------------------------
//  Inline x86 hooks (5-byte rel jmp + trampoline with re-encoded rel branches)
// ---------------------------------------------------------------------------
bool   HookInstall(void* target, void* detour, void** trampoline,
                   bool suspendOtherThreads = true);
bool   HookRemove(void* target);
void   HookRemoveAll();
size_t HookDecodeLength(const uint8_t* code, size_t maxLen);

// ---------------------------------------------------------------------------
//  IAT hooks
// ---------------------------------------------------------------------------
bool IatHook(HMODULE module, const char* importDll, const char* funcName,
             void* detour, void** original);
bool IatUnhook(HMODULE module, const char* importDll, const char* funcName,
               void* original);
void IatUnhookAll();

// ---------------------------------------------------------------------------
//  Hotkey polling (GetAsyncKeyState; "all keys held" rising edge)
// ---------------------------------------------------------------------------
constexpr int kMaxHotkeys = 8;
void HotkeyConfigure(const int* vks, int count);
int  HotkeyCount();
bool HotkeyAllHeldRisingEdge();

} // namespace harness
