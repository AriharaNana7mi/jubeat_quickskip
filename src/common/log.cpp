// ---------------------------------------------------------------------------
//  log.cpp - append-only, mutex protected file logger.
//
//  Design notes:
//   * opens / appends / closes the file for every line.  That makes the log
//     readable while the game is running and avoids fd leaks on teardown.
//   * uses only GetLocalTime + CreateFileW + WriteFile, so it is safe to call
//     from inside a detour of Sleep / GetTickCount / etc.
//   * recursion guard drops nested log calls (e.g. if the CRT formatting below
//     ever ends up calling a hooked API).
// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdarg>

namespace harness {
namespace {

INIT_ONCE      g_csOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_cs;
wchar_t        g_logPath[MAX_PATH * 2] = {};
volatile LONG  g_inLog = 0;

BOOL CALLBACK InitCsOnce(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

void EnsureCs() {
    InitOnceExecuteOnce(&g_csOnce, InitCsOnce, nullptr, nullptr);
}

bool TryOpenAppend(HANDLE* out) {
    if (!g_logPath[0]) return false;
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    *out = h;
    return true;
}

void WriteLineRaw(const char* text) {
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!TryOpenAppend(&h)) return;
    DWORD written = 0;
    WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
    CloseHandle(h);
}

} // namespace

void LogInit(HMODULE self) {
    EnsureCs();
    if (g_logPath[0]) return;

    std::wstring path;
    wchar_t env[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"JUBEAT_HOOK_LOG", env, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        path = env;
    } else {
        wchar_t selfPath[MAX_PATH * 2] = {};
        if (GetModuleFileNameW(self, selfPath, MAX_PATH * 2)) {
            std::wstring selfW(selfPath);
            size_t slash = selfW.find_last_of(L"\\/");
            std::wstring dir =
                slash != std::wstring::npos ? selfW.substr(0, slash) : L".";
            std::wstring file =
                slash != std::wstring::npos ? selfW.substr(slash + 1) : selfW;
            size_t dot = file.find_last_of(L'.');
            if (dot != std::wstring::npos) file.resize(dot);
            std::wstring logDir = dir + L"\\logs";
            CreateDirectoryW(logDir.c_str(), nullptr);
            // default log name follows the DLL name:
            //   hook_dll.dll -> logs\hook_dll.log
            //   jubeat_probe.dll -> logs\jubeat_probe.log
            path = logDir + L"\\" + file + L".log";
        }
    }
    if (path.empty()) {
        wchar_t tmp[MAX_PATH * 2] = {};
        GetTempPathW(MAX_PATH * 2, tmp);
        path = std::wstring(tmp) + L"jubeat_hook_" +
               std::to_wstring(GetCurrentProcessId()) + L".log";
    }

    wcsncpy(g_logPath, path.c_str(), MAX_PATH * 2 - 1);
    g_logPath[MAX_PATH * 2 - 1] = 0;

    HANDLE h = INVALID_HANDLE_VALUE;
    if (!TryOpenAppend(&h)) {
        // fall back to %TEMP%
        wchar_t tmp[MAX_PATH * 2] = {};
        GetTempPathW(MAX_PATH * 2, tmp);
        std::wstring fallback = std::wstring(tmp) + L"jubeat_hook_" +
                                std::to_wstring(GetCurrentProcessId()) + L".log";
        wcsncpy(g_logPath, fallback.c_str(), MAX_PATH * 2 - 1);
        g_logPath[MAX_PATH * 2 - 1] = 0;
        TryOpenAppend(&h);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void LogShutdown() {
    // nothing to flush: every line is closed after writing
}

const wchar_t* LogPath() { return g_logPath; }

void LogLine(const char* text) {
    if (!text) return;
    EnsureCs();
    if (InterlockedExchange(&g_inLog, 1) != 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[4096];
    int pos = snprintf(line, sizeof(line),
                       "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu tid=%lu | ",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, GetCurrentProcessId(),
                       GetCurrentThreadId());
    if (pos < 0) pos = 0;
    size_t used = (size_t)pos < sizeof(line) - 3 ? (size_t)pos : sizeof(line) - 3;
    size_t room = sizeof(line) - used - 1;
    size_t tlen = strlen(text);
    if (tlen > room) tlen = room;
    memcpy(line + used, text, tlen);
    used += tlen;
    line[used++] = '\r';
    line[used++] = '\n';
    line[used] = 0;

    WriteLineRaw(line);
    InterlockedExchange(&g_inLog, 0);
}

void Logf(const char* fmt, ...) {
    char buf[3072];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogLine(buf);
}

} // namespace harness
