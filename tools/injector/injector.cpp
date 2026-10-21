// ---------------------------------------------------------------------------
//  injector.exe - 32-bit DLL injector for 32-bit Windows targets.
//
//  Modes
//  -----
//    launch : injector.exe --dll hook_dll.dll [options] target.exe [args...]
//             CreateProcess(CREATE_SUSPENDED) -> resume -> (wait) -> inject
//    attach : injector.exe --dll hook_dll.dll -p <pid> [options]
//    unload : injector.exe -p <pid> --unload --module hook_dll.dll
//    call   : injector.exe -p <pid> --call HarnessPing --module hook_dll.dll
//                                                    [--arg <dword>]
//
//  Injection mechanism: VirtualAllocEx + WriteProcessMemory +
//  CreateRemoteThread(LoadLibraryA).  The remote LoadLibraryA address is
//  resolved *inside the target* (PEB module walk + remote export table parse),
//  not assumed to be equal to our own address.  Falls back to the local
//  address (valid for KnownDLLs on the same boot) if the walk fails.
//
//  32-bit only: the injector is a PE32 image; a 64-bit target is refused.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
//  console + file logging
// ---------------------------------------------------------------------------
FILE*       g_logFile = nullptr;
std::string g_exeDir;

std::string GetExeDir() {
    char path[MAX_PATH * 2] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH * 2);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s.resize(slash);
    return s;
}

void InitLog() {
    g_exeDir = GetExeDir();
    std::string logDir = g_exeDir + "\\logs";
    CreateDirectoryA(logDir.c_str(), nullptr);
    std::string path = logDir + "\\injector.log";
    g_logFile = fopen(path.c_str(), "a");
}

void ILog(const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[4300];
    snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, msg);
    fputs(line, stdout);
    fflush(stdout);
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------
bool NameEqI(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

bool FileExistsA(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FullPath(const std::string& p) {
    char buf[MAX_PATH * 4] = {};
    DWORD n = GetFullPathNameA(p.c_str(), MAX_PATH * 4, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 4) return p;
    return std::string(buf);
}

std::string DirName(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    if (s == std::string::npos) return std::string();
    return p.substr(0, s);
}

std::string BaseName(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    if (s == std::string::npos) return p;
    return p.substr(s + 1);
}

std::string QuoteArg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}

// Reads PE machine + "is DLL" bit from an on-disk PE file.
bool ReadPeMachine(const std::string& path, WORD* machine, bool* isDll) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        unsigned char dos[0x40];
        if (fread(dos, 1, sizeof(dos), f) != sizeof(dos)) break;
        if (dos[0] != 'M' || dos[1] != 'Z') break;
        LONG peOff = *(LONG*)(dos + 0x3c);
        if (peOff <= 0) break;
        if (fseek(f, peOff, SEEK_SET) != 0) break;
        unsigned char nt[24];
        if (fread(nt, 1, sizeof(nt), f) != sizeof(nt)) break;
        if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) break;
        *machine = (WORD)(nt[4] | (nt[5] << 8));
        WORD characteristics = (WORD)(nt[22] | (nt[23] << 8));
        if (isDll) *isDll = (characteristics & IMAGE_FILE_DLL) != 0;
        ok = true;
    } while (0);
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
//  remote memory / PEB walk / remote export resolution (32-bit target)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct UNICODE_STRING32 {
    USHORT Length;
    USHORT MaximumLength;
    DWORD  Buffer;
};
struct LIST_ENTRY32 {
    DWORD Flink;
    DWORD Blink;
};
struct LDR_DATA_TABLE_ENTRY32 {
    LIST_ENTRY32    InLoadOrderLinks;              // 0x00
    LIST_ENTRY32    InMemoryOrderLinks;            // 0x08
    LIST_ENTRY32    InInitializationOrderLinks;    // 0x10
    DWORD           DllBase;                       // 0x18
    DWORD           EntryPoint;                    // 0x1c
    DWORD           SizeOfImage;                   // 0x20
    UNICODE_STRING32 FullDllName;                  // 0x24
    UNICODE_STRING32 BaseDllName;                  // 0x2c
};
#pragma pack(pop)

struct RemoteModule {
    uint32_t base = 0;
    std::string name;
};

bool ReadRemote(HANDLE hProc, uint32_t addr, void* buf, size_t size) {
    if (!addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(hProc, (LPCVOID)(uintptr_t)addr, buf, size,
                             &got) != 0 &&
           got == size;
}

typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG,
                                                 PULONG);

uint32_t GetRemotePeb(HANDLE hProc) {
    static NtQueryInformationProcessFn fn = nullptr;
    if (!fn) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll)
            fn = (NtQueryInformationProcessFn)GetProcAddress(
                ntdll, "NtQueryInformationProcess");
    }
    if (!fn) return 0;
    struct PROCESS_BASIC_INFORMATION {
        PVOID     Reserved1;
        PVOID     PebBaseAddress;
        PVOID     Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID     Reserved3;
    } pbi;
    memset(&pbi, 0, sizeof(pbi));
    ULONG retLen = 0;
    LONG st = fn(hProc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi),
                 &retLen);
    if (st != 0) return 0;
    return (uint32_t)(uintptr_t)pbi.PebBaseAddress;
}

std::vector<RemoteModule> WalkRemoteModules(HANDLE hProc) {
    std::vector<RemoteModule> mods;
    uint32_t peb = GetRemotePeb(hProc);
    if (!peb) return mods;
    uint32_t ldr = 0;
    if (!ReadRemote(hProc, peb + 0x0C, &ldr, 4) || !ldr) return mods;
    uint32_t head = ldr + 0x14; // PEB_LDR_DATA.InMemoryOrderModuleList
    uint32_t cur = 0;
    if (!ReadRemote(hProc, head, &cur, 4)) return mods;
    for (int guard = 0; cur && cur != head && guard < 2048; ++guard) {
        uint32_t entryAddr = cur - offsetof(LDR_DATA_TABLE_ENTRY32,
                                            InMemoryOrderLinks);
        LDR_DATA_TABLE_ENTRY32 e;
        memset(&e, 0, sizeof(e));
        if (!ReadRemote(hProc, entryAddr, &e, sizeof(e))) break;
        RemoteModule m;
        m.base = e.DllBase;
        if (e.BaseDllName.Length && e.BaseDllName.Buffer) {
            wchar_t w[520] = {};
            uint32_t bytes = e.BaseDllName.Length;
            if (bytes > sizeof(w) - 2) bytes = (uint32_t)(sizeof(w) - 2);
            if (ReadRemote(hProc, e.BaseDllName.Buffer, w, bytes)) {
                char a[520] = {};
                int n = WideCharToMultiByte(CP_ACP, 0, w, (int)(bytes / 2), a,
                                            (int)sizeof(a) - 1, nullptr,
                                            nullptr);
                if (n > 0) m.name.assign(a, (size_t)n);
            }
        }
        mods.push_back(std::move(m));
        uint32_t next = 0;
        if (!ReadRemote(hProc, cur, &next, 4)) break;
        cur = next;
    }
    return mods;
}

uint32_t RemoteModuleBase(HANDLE hProc, const char* moduleName) {
    std::vector<RemoteModule> mods = WalkRemoteModules(hProc);
    for (const auto& m : mods) {
        if (NameEqI(m.name.c_str(), moduleName)) return m.base;
    }
    return 0;
}

uint32_t RemoteExport(HANDLE hProc, const char* moduleName,
                      const char* funcName) {
    std::vector<RemoteModule> mods = WalkRemoteModules(hProc);
    uint32_t base = 0;
    for (const auto& m : mods) {
        if (NameEqI(m.name.c_str(), moduleName)) {
            base = m.base;
            break;
        }
    }
    if (!base) return 0;

    IMAGE_DOS_HEADER dos;
    if (!ReadRemote(hProc, base, &dos, sizeof(dos))) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS32 nt;
    if (!ReadRemote(hProc, base + (uint32_t)dos.e_lfanew, &nt, sizeof(nt)))
        return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD expRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
                       .VirtualAddress;
    if (!expRva) return 0;
    IMAGE_EXPORT_DIRECTORY exp;
    if (!ReadRemote(hProc, base + expRva, &exp, sizeof(exp))) return 0;

    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        DWORD nameRva = 0;
        if (!ReadRemote(hProc, base + exp.AddressOfNames + i * 4, &nameRva, 4))
            break;
        char name[256] = {};
        if (!ReadRemote(hProc, base + nameRva, name, sizeof(name) - 1))
            continue;
        if (!NameEqI(name, funcName)) continue;
        WORD ordinal = 0;
        if (!ReadRemote(hProc, base + exp.AddressOfNameOrdinals + i * 2,
                        &ordinal, 2))
            return 0;
        DWORD fnRva = 0;
        if (!ReadRemote(hProc, base + exp.AddressOfFunctions + ordinal * 4,
                        &fnRva, 4))
            return 0;
        return base + fnRva;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  target process helpers
// ---------------------------------------------------------------------------
HANDLE OpenTarget(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                               PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) {
        ILog("ERROR: OpenProcess(pid=%lu) failed, err=%lu. Try running the "
             "injector as administrator.",
             (unsigned long)pid, (unsigned long)GetLastError());
    }
    return h;
}

bool IsTarget64Bit(HANDLE hProc) {
    BOOL targetWow = FALSE;
    if (!IsWow64Process(hProc, &targetWow)) return false;
    BOOL selfWow = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow);
    if (!selfWow) return false; // 32-bit OS: 64-bit targets cannot exist
    return targetWow == FALSE;
}

// ---------------------------------------------------------------------------
//  the three remote operations
// ---------------------------------------------------------------------------
bool DoInject(HANDLE hProc, DWORD pid, const std::string& dllPath,
              DWORD waitForLoaderMs = 5000) {
    ILog("inject: target pid=%lu dll=%s", (unsigned long)pid, dllPath.c_str());

    // A freshly resumed process may not have completed loader initialisation
    // yet (PEB->Ldr is still NULL), so retry the remote module walk for a
    // moment before falling back to our own LoadLibraryA address.
    uint32_t k32 = 0;
    uint32_t loadLibraryA = 0;
    DWORD waited = 0;
    const DWORD step = 100;
    bool announced = false;
    while (waited <= waitForLoaderMs) {
        k32 = RemoteModuleBase(hProc, "kernel32.dll");
        if (k32) {
            loadLibraryA = RemoteExport(hProc, "kernel32.dll", "LoadLibraryA");
            if (loadLibraryA) break;
        }
        if (!announced && waitForLoaderMs > 0) {
            ILog("inject: target loader not ready for PEB walk yet, retrying "
                 "(up to %lu ms)...",
                 (unsigned long)waitForLoaderMs);
            announced = true;
        }
        Sleep(step);
        waited += step;
    }
    if (k32) {
        ILog("inject: remote kernel32.dll base=0x%08X (PEB walk, waited %lu ms)",
             k32, (unsigned long)waited);
    } else {
        ILog("inject: WARN PEB module walk failed after %lu ms",
             (unsigned long)waited);
    }

    if (loadLibraryA) {
        ILog("inject: remote LoadLibraryA=0x%08X (resolved in target, no "
             "assumption about ASLR)",
             loadLibraryA);
    } else {
        HMODULE localK32 = GetModuleHandleA("kernel32.dll");
        loadLibraryA =
            (uint32_t)(uintptr_t)GetProcAddress(localK32, "LoadLibraryA");
        ILog("inject: WARN remote export resolve failed, fallback to local "
             "LoadLibraryA=0x%08X (same-boot KnownDLL assumption)",
             loadLibraryA);
        if (!loadLibraryA) {
            ILog("inject: ERROR LoadLibraryA address 0");
            return false;
        }
    }

    SIZE_T len = dllPath.size() + 1;
    void* remotePath = VirtualAllocEx(hProc, nullptr, len,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        ILog("inject: ERROR VirtualAllocEx(%zu) failed, err=%lu",
             (size_t)len, (unsigned long)GetLastError());
        return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, remotePath, dllPath.c_str(), len, &written) ||
        written != len) {
        ILog("inject: ERROR WriteProcessMemory failed, err=%lu",
             (unsigned long)GetLastError());
        VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
        return false;
    }
    ILog("inject: wrote dll path to target @ %p", remotePath);

    DWORD tid = 0;
    HANDLE th = CreateRemoteThread(
        hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)(uintptr_t)loadLibraryA,
        remotePath, 0, &tid);
    if (!th) {
        ILog("inject: ERROR CreateRemoteThread(LoadLibraryA) failed, err=%lu",
             (unsigned long)GetLastError());
        VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
        return false;
    }
    ILog("inject: remote thread tid=%lu started, waiting for LoadLibraryA...",
         (unsigned long)tid);

    DWORD wr = WaitForSingleObject(th, 60000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);

    if (wr == WAIT_TIMEOUT) {
        ILog("inject: ERROR remote LoadLibraryA did not return within 60s "
             "(thread possibly stuck in target loader)");
        return false;
    }
    if (exitCode == 0) {
        ILog("inject: ERROR remote LoadLibraryA returned NULL. Common causes: "
             "target bitness mismatch, DLL dependency missing, or DLL init "
             "failed. LoadLibraryA error code is not retrievable remotely.");
        return false;
    }
    ILog("inject: SUCCESS remote HMODULE=0x%08X (wait=%lu)", exitCode,
         (unsigned long)wr);

    // Confirm from the target's own module list that the DLL really is loaded
    // and report its base address (also proves the PEB walk works post-load).
    std::string dllName = BaseName(dllPath);
    for (int i = 0; i < 10; ++i) {
        std::vector<RemoteModule> mods = WalkRemoteModules(hProc);
        for (const auto& m : mods) {
            if (NameEqI(m.name.c_str(), dllName.c_str())) {
                ILog("inject: verified in target module list: %s base=0x%08X",
                     m.name.c_str(), m.base);
                return true;
            }
        }
        Sleep(100);
    }
    ILog("inject: WARN injected DLL not found in target module list yet "
         "(LoadLibrary returned 0x%08X though)",
         exitCode);
    return true;
}

bool DoUnload(HANDLE hProc, DWORD pid, const std::string& moduleName) {
    std::vector<RemoteModule> mods = WalkRemoteModules(hProc);
    uint32_t base = 0;
    for (const auto& m : mods) {
        if (NameEqI(m.name.c_str(), moduleName.c_str())) {
            base = m.base;
            break;
        }
    }
    if (!base) {
        ILog("unload: module %s not loaded in pid=%lu", moduleName.c_str(),
             (unsigned long)pid);
        return false;
    }
    ILog("unload: module %s base=0x%08X", moduleName.c_str(), base);
    uint32_t freeLibrary = RemoteExport(hProc, "kernel32.dll", "FreeLibrary");
    if (!freeLibrary) {
        ILog("unload: ERROR could not resolve FreeLibrary in target");
        return false;
    }
    DWORD tid = 0;
    HANDLE th = CreateRemoteThread(
        hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)(uintptr_t)freeLibrary,
        (LPVOID)(uintptr_t)base, 0, &tid);
    if (!th) {
        ILog("unload: ERROR CreateRemoteThread(FreeLibrary) failed, err=%lu",
             (unsigned long)GetLastError());
        return false;
    }
    DWORD wr = WaitForSingleObject(th, 30000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    if (wr == WAIT_TIMEOUT || exitCode == 0) {
        ILog("unload: ERROR FreeLibrary failed (wait=%lu exit=%lu)",
             (unsigned long)wr, (unsigned long)exitCode);
        return false;
    }
    ILog("unload: SUCCESS module %s unloaded", moduleName.c_str());
    return true;
}

bool DoCall(HANDLE hProc, DWORD pid, const std::string& moduleName,
            const std::string& exportName, DWORD arg) {
    uint32_t fn = RemoteExport(hProc, moduleName.c_str(), exportName.c_str());
    if (!fn) {
        ILog("call: ERROR export %s!%s not found in pid=%lu (is the DLL "
             "injected?)",
             moduleName.c_str(), exportName.c_str(), (unsigned long)pid);
        return false;
    }
    ILog("call: %s!%s @ 0x%08X arg=%lu", moduleName.c_str(),
         exportName.c_str(), fn, (unsigned long)arg);
    DWORD tid = 0;
    HANDLE th = CreateRemoteThread(hProc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)(uintptr_t)fn,
                                   (LPVOID)(uintptr_t)arg, 0, &tid);
    if (!th) {
        ILog("call: ERROR CreateRemoteThread failed, err=%lu",
             (unsigned long)GetLastError());
        return false;
    }
    DWORD wr = WaitForSingleObject(th, 30000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    if (wr == WAIT_TIMEOUT) {
        ILog("call: ERROR remote call timed out");
        return false;
    }
    ILog("call: returned %lu", (unsigned long)exitCode);
    return true;
}

// ---------------------------------------------------------------------------
//  modes
// ---------------------------------------------------------------------------
struct Options {
    std::string              dllPath;
    std::string              exePath;
    std::vector<std::string> exeArgs;
    std::string              moduleName;
    std::string              callExport;
    DWORD                    pid       = 0;
    int                      waitMs    = 0;
    DWORD                    callArg   = 0;
    bool                     injectSuspended = false;
    bool                     unload    = false;
    bool                     call      = false;
    bool                     newConsole = false;
};

void PrintUsage() {
    printf(
        "injector.exe - 32-bit DLL injector for 32-bit Windows targets\n"
        "\n"
        "usage:\n"
        "  launch:  injector.exe --dll <hook.dll> [options] -- <target.exe> "
        "[args...]\n"
        "  attach:  injector.exe --dll <hook.dll> -p <pid> [options]\n"
        "  unload:  injector.exe -p <pid> --unload --module <hook.dll>\n"
        "  call:    injector.exe -p <pid> --call <ExportName> --module "
        "<hook.dll> [--arg <dword>]\n"
        "\n"
        "options:\n"
        "  -d, --dll <path>    DLL to inject (must be PE32/i386)\n"
        "  -p, --pid <pid>     attach to an already running process\n"
        "  -w, --wait <ms>     wait this long after resuming the target "
        "before injecting\n"
        "      --suspended     inject while the new process is still "
        "suspended (then resume)\n"
        "      --new-console   give a launched console target its own console "
        "window\n"
        "      --unload        FreeLibrary the module in the target\n"
        "      --module <name> target module name for --unload/--call "
        "(default: dll basename)\n"
        "      --call <export> call an exported function (DWORD WINAPI fn(DWORD)) "
        "in the target\n"
        "      --arg <dword>   argument passed to --call (decimal or 0x..)\n"
        "  -h, --help          this help\n"
        "\n"
        "exit codes: 0 ok, 1 usage/argument, 2 open process, 3 bad dll, "
        "4 remote alloc, 5 remote thread, 6 inject failed, 7 unload failed, "
        "8 call failed\n");
}

int DoLaunch(const Options& o) {
    std::string exeFull = FullPath(o.exePath);
    std::string dllFull = FullPath(o.dllPath);
    if (!FileExistsA(exeFull)) {
        ILog("launch: ERROR target not found: %s", exeFull.c_str());
        return 1;
    }
    if (!FileExistsA(dllFull)) {
        ILog("launch: ERROR dll not found: %s", dllFull.c_str());
        return 3;
    }
    WORD machine = 0;
    bool isDll = false;
    if (!ReadPeMachine(exeFull, &machine, &isDll) || isDll) {
        ILog("launch: ERROR not a valid PE executable: %s", exeFull.c_str());
        return 1;
    }
    if (machine == IMAGE_FILE_MACHINE_AMD64) {
        ILog("launch: ERROR target is 64-bit (PE32+); this injector is 32-bit "
             "and cannot inject it: %s",
             exeFull.c_str());
        return 1;
    }
    if (machine != IMAGE_FILE_MACHINE_I386) {
        ILog("launch: ERROR target machine 0x%04X is not i386",
             (unsigned)machine);
        return 1;
    }
    WORD dllMachine = 0;
    bool dllIsDll = false;
    if (!ReadPeMachine(dllFull, &dllMachine, &dllIsDll) || !dllIsDll) {
        ILog("launch: ERROR not a valid PE DLL: %s", dllFull.c_str());
        return 3;
    }
    if (dllMachine != IMAGE_FILE_MACHINE_I386) {
        ILog("launch: ERROR dll machine 0x%04X is not i386 (PE32 required)",
             (unsigned)dllMachine);
        return 3;
    }

    std::string cmdLine = QuoteArg(exeFull);
    for (const auto& a : o.exeArgs) cmdLine += " " + QuoteArg(a);
    std::string workDir = DirName(exeFull);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    DWORD flags = CREATE_SUSPENDED;
    if (o.newConsole) flags |= CREATE_NEW_CONSOLE;

    ILog("launch: %s", cmdLine.c_str());
    if (!CreateProcessA(exeFull.c_str(), cmdLine.data(), nullptr, nullptr,
                        FALSE, flags, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si,
                        &pi)) {
        ILog("launch: ERROR CreateProcess failed, err=%lu",
             (unsigned long)GetLastError());
        return 1;
    }
    ILog("launch: created pid=%lu suspended", (unsigned long)pi.dwProcessId);

    bool ok = false;
    if (o.injectSuspended) {
        // Target is still in loader init: PEB walk cannot work yet, so inject
        // with the local LoadLibraryA address (KnownDLL) and resume afterwards.
        ok = DoInject(pi.hProcess, pi.dwProcessId, dllFull, 0);
        ResumeThread(pi.hThread);
        ILog("launch: resumed target");
    } else {
        ResumeThread(pi.hThread);
        ILog("launch: resumed target");
        if (o.waitMs > 0) {
            ILog("launch: waiting %d ms before injection", o.waitMs);
            Sleep((DWORD)o.waitMs);
        }
        ok = DoInject(pi.hProcess, pi.dwProcessId, dllFull, 5000);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!ok) return 6;
    ILog("launch: done, target keeps running (pid=%lu)",
         (unsigned long)pi.dwProcessId);
    return 0;
}

int DoAttach(const Options& o) {
    std::string dllFull = FullPath(o.dllPath);
    if (!FileExistsA(dllFull)) {
        ILog("attach: ERROR dll not found: %s", dllFull.c_str());
        return 3;
    }
    WORD dllMachine = 0;
    bool dllIsDll = false;
    if (!ReadPeMachine(dllFull, &dllMachine, &dllIsDll) || !dllIsDll ||
        dllMachine != IMAGE_FILE_MACHINE_I386) {
        ILog("attach: ERROR dll is not a 32-bit PE DLL: %s", dllFull.c_str());
        return 3;
    }
    HANDLE hProc = OpenTarget(o.pid);
    if (!hProc) return 2;
    if (IsTarget64Bit(hProc)) {
        ILog("attach: ERROR target pid=%lu is 64-bit; 32-bit injector cannot "
             "inject it",
             (unsigned long)o.pid);
        CloseHandle(hProc);
        return 2;
    }
    if (o.waitMs > 0) {
        ILog("attach: waiting %d ms before injection", o.waitMs);
        Sleep((DWORD)o.waitMs);
    }
    bool ok = DoInject(hProc, o.pid, dllFull, 5000);
    CloseHandle(hProc);
    return ok ? 0 : 6;
}

int DoUnloadMode(const Options& o) {
    HANDLE hProc = OpenTarget(o.pid);
    if (!hProc) return 2;
    bool ok = DoUnload(hProc, o.pid, o.moduleName);
    CloseHandle(hProc);
    return ok ? 0 : 7;
}

int DoCallMode(const Options& o) {
    HANDLE hProc = OpenTarget(o.pid);
    if (!hProc) return 2;
    bool ok = DoCall(hProc, o.pid, o.moduleName, o.callExport, o.callArg);
    CloseHandle(hProc);
    return ok ? 0 : 8;
}

} // namespace

int main(int argc, char** argv) {
    InitLog();
    ILog("=== injector.exe pid=%lu argc=%d ===", (unsigned long)GetCurrentProcessId(),
         argc);

    Options o;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (a.empty() || a[0] != '-') break; // start of target exe

        auto needValue = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                ILog("ERROR: option %s requires a value", opt);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (a == "-d" || a == "--dll") {
            const char* v = needValue("--dll");
            if (!v) return 1;
            o.dllPath = v;
        } else if (a == "-p" || a == "--pid") {
            const char* v = needValue("--pid");
            if (!v) return 1;
            o.pid = (DWORD)strtoul(v, nullptr, 0);
        } else if (a == "-w" || a == "--wait") {
            const char* v = needValue("--wait");
            if (!v) return 1;
            o.waitMs = atoi(v);
        } else if (a == "--suspended") {
            o.injectSuspended = true;
        } else if (a == "--new-console") {
            o.newConsole = true;
        } else if (a == "--unload") {
            o.unload = true;
        } else if (a == "--module") {
            const char* v = needValue("--module");
            if (!v) return 1;
            o.moduleName = v;
        } else if (a == "-c" || a == "--call") {
            const char* v = needValue("--call");
            if (!v) return 1;
            o.call = true;
            o.callExport = v;
        } else if (a == "--arg") {
            const char* v = needValue("--arg");
            if (!v) return 1;
            o.callArg = (DWORD)strtoul(v, nullptr, 0);
        } else if (a == "-v" || a == "--verbose") {
            // accepted for compatibility; logging is always verbose
        } else {
            ILog("ERROR: unknown option %s", a.c_str());
            PrintUsage();
            return 1;
        }
    }
    if (i < argc) {
        o.exePath = argv[i++];
        for (; i < argc; ++i) o.exeArgs.push_back(argv[i]);
    }

    // default module name = dll basename
    if (!o.dllPath.empty()) o.moduleName = BaseName(FullPath(o.dllPath));

    if (o.unload) {
        if (!o.pid || o.moduleName.empty()) {
            ILog("ERROR: --unload requires -p <pid> and --module <name>");
            PrintUsage();
            return 1;
        }
        return DoUnloadMode(o);
    }
    if (o.call) {
        if (!o.pid || o.moduleName.empty() || o.callExport.empty()) {
            ILog("ERROR: --call requires -p <pid>, --module and --call <export>");
            PrintUsage();
            return 1;
        }
        return DoCallMode(o);
    }
    if (o.pid && !o.exePath.empty()) {
        ILog("ERROR: -p and a target exe are mutually exclusive");
        return 1;
    }
    if (o.pid) {
        if (o.dllPath.empty()) {
            ILog("ERROR: attach mode requires --dll");
            return 1;
        }
        return DoAttach(o);
    }
    if (!o.exePath.empty()) {
        if (o.dllPath.empty()) {
            ILog("ERROR: launch mode requires --dll");
            return 1;
        }
        return DoLaunch(o);
    }
    ILog("ERROR: nothing to do");
    PrintUsage();
    return 1;
}
