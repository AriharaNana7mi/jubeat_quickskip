// ---------------------------------------------------------------------------
//  feature_quickskip.cpp - jubeat Ave (L44) "four-corner quick exit" mod.
//
//  Behaviour
//  ---------
//  While a tune is actually being played (MUSIC_PLAY child state = 0x23886000
//  "PLAYING"), holding a configurable set of panels (default: the four corners
//  1,4,13,16 => bit mask 0x9009, change with "Keys=" in the INI) for HoldMs
//  milliseconds aborts the current tune and returns to the song select screen
//  (MUSIC_SELECT).  The abort walks a short prefix of the game's own post-tune
//  state chain (MUSIC_RESULT -> 0x238D0000 continue page -> junction ->
//  MUSIC_SELECT) so the shared AEP UI data sets are laid out for the select
//  screen; a plain forced jump leaves the play HUD overlaying the select
//  screen in this build (see "abort sequence engine" below).  The chain costs
//  ~2.8 s (AbortMode=15); AbortMode=22/23 shorten it to ~1.7 s / ~1.3 s.
//
//  Mechanism (all addresses verified for jubeat.dll build whose
//  FileHeader.TimeDateStamp = 0x6a59f658, entry RVA 0x210560):
//    * The game's input module runs input.dll!input_update() once per frame on
//      the game thread.  We inline-hook it and chain to the original first, so
//      the gftools input-device state map is already refreshed for this frame.
//    * Held panel bitmap  = gftools.dll!GFInputDDevGetStateMap(2, MapField)
//      (low 16 bits, bit i = panel i+1, row major).  The trigger mask is
//      configurable via "Keys=" (default 0x9009 = panels 1,4,13,16).
//    * Play detection     = *(u32*)(jubeat_base + 0xFCA6A0 + 0x14) == 0x23880000
//                           (MUSIC_LOOP FSM object's current state)
//                           and *(u32*)(jubeat_base + 0xFCA6F8 + 0x14) == 0x23886000
//                           (MUSIC_PLAY FSM object's current state = PLAYING)
//    * Abort              = sound.dll!sound_bgm_stop() etc., then
//                           gftools.dll!GFFSMObjectTransitState(MUSIC_LOOP,
//                              0x238A0000) and a short scripted walk through
//                           the native post-tune chain:
//                             0x238A0000 MUSIC_RESULT (result/HUD re-layout)
//                             0x238D0000 continue page (AEP teardown)
//                             0x23830000 junction -> MUSIC_SELECT
//      The shared play-HUD AEP data sets are only re-laid-out for the select
//      screen by this native chain; a direct jump to MUSIC_SELECT leaves the
//      play HUD ("1ST TUNE"/LEVEL/song row/lane bar) on top of the select
//      screen (verified on the real game).  Mode 1 keeps the old direct jump
//      for debugging only.
//
//  Safety
//  ------
//    * Version is validated at startup (PE timestamp + FSM object ids/names);
//      on mismatch the mod disables itself and only writes a log line.
//    * Every call into game code is wrapped in a vectored-exception-handler +
//      setjmp/longjmp guard, so a bad pointer cannot take the game down.
//    * Hooks are only installed after input.dll is loaded; all game interaction
//      happens on the game thread inside the input_update detour.
//
//  Config: <dll-name>.ini next to the DLL (all keys optional).
// ---------------------------------------------------------------------------
#include "common/common.h"
#include "feature.h"

#include <windows.h>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace quickskip {

using harness::GetModuleRange;
using harness::HookInstall;
using harness::Logf;

namespace {

// ---- jubeat.dll build identity -------------------------------------------
constexpr DWORD    kExpectedTimeStamp = 0x6a59f658;  // jubeat.dll (timestamp)
constexpr uint32_t RVA_OBJ_MUSIC_LOOP = 0x00FCA6A0;  // GFFSMObject (static)
constexpr uint32_t RVA_OBJ_MUSIC_PLAY = 0x00FCA6F8;  // GFFSMObject (static)
constexpr uint32_t OBJ_ID             = 0x00;
constexpr uint32_t OBJ_STATE          = 0x14;
constexpr uint32_t OBJ_NAME_PTR       = 0x48;

constexpr uint32_t ID_MUSIC_LOOP      = 0x23800000;
constexpr uint32_t ID_MUSIC_PLAY      = 0x23880000;
constexpr uint32_t ID_MUSIC_SELECT    = 0x23840000;
constexpr uint32_t ID_MUSIC_RESULT    = 0x238A0000;  // single-tune result screen
constexpr uint32_t ID_MUSIC_JUNCTION  = 0x23830000;  // post-tune router junction
constexpr uint32_t ID_CONTINUE_SELECT = 0x238D0000;  // post-result continue page
constexpr uint32_t STATE_PLAYING      = 0x23886000;
constexpr uint32_t DEFAULT_KEYS       = 0x9009;      // panels 1,4,13,16

// ---- configuration (jubeat_quickskip.ini) --------------------------------
struct Config {
    bool     enabled      = true;
    uint32_t keysMask     = DEFAULT_KEYS; // panels to hold (bit i = panel i+1)
    int      holdMs       = 300;    // how long the keys must be held together
    int      cooldownMs   = 1500;   // min time between two aborts
    int      mapField     = 1;      // GFInputDDevGetStateMap(2, field)
    bool     stopSfx      = true;   // also stop sound effects on abort
    bool     verbose      = false;  // log every state change
    int      abortMode    = 15;     // abort sequence variant (see StartAbort)
};
Config g_cfg;

// ---- modules / resolved functions ---------------------------------------
uintptr_t g_jubeat  = 0;
uintptr_t g_gftools = 0;
uintptr_t g_sound   = 0;
bool      g_ready   = false;   // hooks installed, objects validated
bool      g_disabled = false;  // version mismatch etc.
bool      g_everPlayed = false;

typedef uint32_t(__cdecl* FnGetStateMap)(int dev, int field);
typedef int(__cdecl* FnTransitState)(void* obj, uint32_t state);
typedef void(__cdecl* FnSetNextState)(void* obj, uint32_t state);
typedef void(__cdecl* FnNoArg)(void);
typedef void(__cdecl* FnInputUpdate)(void);

FnGetStateMap   g_getStateMap   = nullptr;
FnTransitState  g_transitState  = nullptr;
FnSetNextState  g_setNextState  = nullptr;
FnNoArg         g_bgmStop       = nullptr;
FnNoArg         g_seStopAll     = nullptr;
FnInputUpdate   g_inputUpdateOrig = nullptr;

void* g_loopObj = nullptr;
void* g_playObj = nullptr;

// ---- trigger state -------------------------------------------------------
uint32_t g_heldSinceMs  = 0;   // when all four corners became held (0 = not held)
uint32_t g_lastAbortMs  = 0;
bool     g_holdConsumed = false;
volatile LONG g_pendingAbort = 0;  // set by the debug export / remote trigger

bool     g_objectsReady = false;   // FSM singleton objects validated (lazy)
uint32_t g_abortAtMs = 0;          // when the last abort was requested
int      g_abortStage = 0;         // 0 idle, 1 waiting for select, 2 waiting result,
                                   // 3 waiting junction
uint32_t g_lastObjectTryMs = 0;    // rate limit for lazy validation
uint32_t g_lastModuleTryMs = 0;    // rate limit for module waiting logs

// abort sequence engine state (AbortMode / remote trigger arg)
volatile LONG g_modeOverride = 0;  // 0 = use INI AbortMode
int      g_seqStage = 0;           // see Stage enum below
uint32_t g_seqAtMs = 0;
uint32_t g_seqFrames = 0;
int      g_seqMode = 0;

enum SeqStage : int {
    SEQ_NONE = 0,
    SEQ_WAIT_RESULT,          // waiting for MUSIC_LOOP to enter MUSIC_RESULT
    SEQ_CHAIN_C,              // result shown: wait then -> 0x238D0000
    SEQ_CHAIN_D               // continue page shown: wait then -> junction
};

// ---------------------------------------------------------------------------
//  SEH-ish guard: vectored exception handler + setjmp/longjmp (GCC on i686
//  does not support MSVC __try/__except in dynamically loaded DLLs).
// ---------------------------------------------------------------------------
struct GuardState {
    jmp_buf* jmp;
    LONG     active;
    DWORD    code;
    void*    addr;
};
DWORD g_guardTls = TLS_OUT_OF_INDEXES;
volatile LONG g_vehInstalled = 0;

GuardState* GuardStateForThread(bool create) {
    if (g_guardTls == TLS_OUT_OF_INDEXES) return nullptr;
    GuardState* gs = (GuardState*)TlsGetValue(g_guardTls);
    if (!gs && create) {
        gs = (GuardState*)calloc(1, sizeof(GuardState));
        if (gs) TlsSetValue(g_guardTls, gs);
    }
    return gs;
}

bool IsGuardableException(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            return true;
        default:
            return false;
    }
}

LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* ep) {
    GuardState* gs = GuardStateForThread(false);
    if (!gs || !gs->active) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsGuardableException(code)) return EXCEPTION_CONTINUE_SEARCH;
    gs->code = code;
    gs->addr = ep->ExceptionRecord->ExceptionAddress;
    gs->active = 0;
    longjmp(*gs->jmp, 1);
    return EXCEPTION_CONTINUE_SEARCH;  // not reached
}

void EnsureGuard() {
    if (InterlockedCompareExchange(&g_vehInstalled, 1, 0) == 0) {
        if (g_guardTls == TLS_OUT_OF_INDEXES) g_guardTls = TlsAlloc();
        if (!AddVectoredExceptionHandler(1, GuardVeh)) {
            Logf("quickskip: ERROR AddVectoredExceptionHandler failed err=%lu",
                 (unsigned long)GetLastError());
            InterlockedExchange(&g_vehInstalled, 0);
        }
    }
}

template <typename Fn>
bool GuardedCall(const char* what, Fn fn) {
    GuardState* gs = GuardStateForThread(true);
    if (!gs) {
        Logf("quickskip: WARN guard state unavailable for %s, skipped", what);
        return false;
    }
    jmp_buf jb;
    jmp_buf* prevJmp = gs->jmp;
    gs->jmp = &jb;
    gs->active = 1;
    if (setjmp(jb) != 0) {
        gs->active = 0;
        gs->jmp = prevJmp;
        Logf("quickskip: SEH caught exception 0x%08X at %p in %s "
             "(call abandoned)", (unsigned)gs->code, gs->addr, what);
        return false;
    }
    fn();
    gs->active = 0;
    gs->jmp = prevJmp;
    return true;
}

// ---------------------------------------------------------------------------
//  memory helpers
// ---------------------------------------------------------------------------
bool IsReadableRange(uintptr_t addr, size_t size) {
    if (!addr || !size) return false;
    uintptr_t p = addr;
    uintptr_t end = addr + size;
    if (end < addr) return false;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                   PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & ok)) return false;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

bool SafeReadU32(uintptr_t addr, uint32_t* out) {
    if (!out || !IsReadableRange(addr, 4)) return false;
    memcpy(out, (const void*)addr, 4);
    return true;
}

bool SafeReadCString(uintptr_t addr, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!addr) return false;
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        if (!IsReadableRange(addr + i, 1)) break;
        memcpy(&c, (const void*)(addr + i), 1);
        out[i++] = c;
        if (c == 0) return true;
    }
    out[i] = 0;
    return i > 0;
}

bool ReadLoadedPeTimeStamp(HMODULE mod, DWORD* ts) {
    if (!mod) return false;
    const uint8_t* p = (const uint8_t*)mod;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)p;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (ts) *ts = nt->FileHeader.TimeDateStamp;
    return true;
}

// ---------------------------------------------------------------------------
//  config file
// ---------------------------------------------------------------------------
std::wstring OwnModulePath() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&OwnModulePath, &self);
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(self, path, MAX_PATH * 2);
    return std::wstring(path);
}

void Trim(std::string* s) {
    size_t a = s->find_first_not_of(" \t\r\n");
    size_t b = s->find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        s->clear();
        return;
    }
    *s = s->substr(a, b - a + 1);
}

std::string DecodePanels(uint32_t mask);  // defined below

// Parse the "Keys=" value: either a hex bitmask ("0x9009") or a comma/space
// separated list of panel numbers ("1,4,13,16" / "1 4 13 16").  Bit i of the
// returned mask means panel i+1 (1..16).  Invalid/empty input keeps the
// default (four corners).
uint32_t ParseKeys(const std::string& value, uint32_t def) {
    std::string t;
    for (char c : value) {
        if (!isspace((unsigned char)c)) t += c;
    }
    if (t.empty()) return def;

    uint32_t mask = 0;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        char* end = nullptr;
        unsigned long x = strtoul(t.c_str(), &end, 16);
        if (end && *end == 0) mask = (uint32_t)(x & 0xFFFFu);
    } else {
        const char* p = t.c_str();
        while (*p) {
            while (*p && !isdigit((unsigned char)*p)) ++p;
            if (!*p) break;
            char* end = nullptr;
            long n = strtol(p, &end, 10);
            if (end == p) break;
            if (n >= 1 && n <= 16) mask |= (1u << (n - 1));
            p = end;
        }
    }
    if (mask == 0) {
        Logf("quickskip: WARN invalid Keys value '%s', using default 0x%04X",
             value.c_str(), (unsigned)def);
        return def;
    }
    return mask;
}

void LoadConfig() {
    std::wstring ini = OwnModulePath();
    size_t dot = ini.find_last_of(L'.');
    size_t slash = ini.find_last_of(L"\\/");
    if (dot != std::wstring::npos &&
        (slash == std::wstring::npos || dot > slash)) {
        ini.resize(dot);
    }
    ini += L".ini";
    FILE* f = _wfopen(ini.c_str(), L"rb");
    if (!f) {
        Logf("quickskip: no config file (%ls), using defaults", ini.c_str());
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        size_t hash = s.find_first_of("#;");
        if (hash != std::string::npos) s.resize(hash);
        Trim(&s);
        if (s.empty() || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        Trim(&key);
        Trim(&val);
        for (char& c : key) c = (char)tolower((unsigned char)c);
        long v = strtol(val.c_str(), nullptr, 0);
        if (key == "enabled")        g_cfg.enabled = v != 0;
        else if (key == "keys")      g_cfg.keysMask = ParseKeys(val, DEFAULT_KEYS);
        else if (key == "holdms")    g_cfg.holdMs = (int)v;
        else if (key == "cooldownms")g_cfg.cooldownMs = (int)v;
        else if (key == "mapfield")  g_cfg.mapField = (int)v;
        else if (key == "stopsfx")   g_cfg.stopSfx = v != 0;
        else if (key == "abortmode") g_cfg.abortMode = (int)v;
        else if (key == "verbose")   g_cfg.verbose = v != 0;
        else Logf("quickskip: WARN unknown config key '%s'", key.c_str());
    }
    fclose(f);
    Logf("quickskip: config enabled=%d keys=0x%04X panels=[%s] holdMs=%d "
         "cooldownMs=%d mapField=%d stopSfx=%d verbose=%d abortMode=%d",
         (int)g_cfg.enabled, g_cfg.keysMask,
         DecodePanels(g_cfg.keysMask).c_str(), g_cfg.holdMs, g_cfg.cooldownMs,
         g_cfg.mapField, (int)g_cfg.stopSfx, (int)g_cfg.verbose,
         g_cfg.abortMode);
}

// ---------------------------------------------------------------------------
//  panel decoding / trigger
// ---------------------------------------------------------------------------
std::string DecodePanels(uint32_t mask) {
    std::string s;
    for (int bit = 0; bit < 16; ++bit) {
        if (mask & (1u << bit)) {
            if (!s.empty()) s += ",";
            char b[8];
            snprintf(b, sizeof(b), "%d", bit + 1);
            s += b;
        }
    }
    return s.empty() ? std::string("-") : s;
}

uint32_t NowMs() { return GetTickCount(); }

bool ReadPanelMask(uint32_t* mask) {
    if (!g_getStateMap) return false;
    uint32_t m = 0;
    FnGetStateMap fn = g_getStateMap;
    int field = g_cfg.mapField;
    bool ok = GuardedCall("GFInputDDevGetStateMap", [fn, field, &m]() {
        m = fn(2, field);
    });
    if (!ok) return false;
    *mask = m & 0xFFFFu;
    return true;
}

// Returns true when all configured trigger panels are held together.
bool TriggerHeld() {
    uint32_t mask = 0;
    if (!ReadPanelMask(&mask)) return false;
    return (mask & g_cfg.keysMask) == g_cfg.keysMask;
}

// Current tune is being played: MUSIC_LOOP in MUSIC_PLAY state and the
// MUSIC_PLAY child is in its PLAYING sub-state.
bool InSongPlay() {
    uint32_t loopState = 0, playId = 0, playState = 0;
    if (!g_objectsReady || !g_loopObj || !g_playObj) return false;
    if (!SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &loopState)) return false;
    if (loopState != ID_MUSIC_PLAY) return false;
    // The MUSIC_PLAY singleton object is (re)attached when a tune starts and
    // detached when the tune ends; require it to really be MUSIC_PLAY.
    if (!SafeReadU32((uintptr_t)g_playObj + OBJ_ID, &playId)) return false;
    if (playId != ID_MUSIC_PLAY) return false;
    if (!SafeReadU32((uintptr_t)g_playObj + OBJ_STATE, &playState)) return false;
    return playState == STATE_PLAYING;
}

// ---------------------------------------------------------------------------
//  deferred validation of the jubeat FSM singleton objects
//
//  MUSIC_LOOP / MUSIC_PLAY static objects exist from process start but only get
//  their ids when the game enters the play loop (song select), which can take
//  well over a minute after boot.  So validation is done lazily, rate limited,
//  and never gives up: until the objects are ready the mod simply does nothing.
// ---------------------------------------------------------------------------
int ValidateObjects();  // defined below

void ValidateObjectsDeferred() {
    if (g_objectsReady || g_disabled) return;
    uint32_t now = NowMs();
    if (g_lastObjectTryMs != 0 && (now - g_lastObjectTryMs) < 500) return;
    g_lastObjectTryMs = now;
    int r = ValidateObjects();
    if (r > 0) {
        g_objectsReady = true;
        Logf("quickskip: FSM objects READY (play detection active)");
    } else if (r < 0) {
        Logf("quickskip: ERROR FSM object validation failed, disabling");
        g_disabled = true;
    }
}

// ---------------------------------------------------------------------------
//  post-abort verification (for sequences that should end in MUSIC_SELECT)
// ---------------------------------------------------------------------------
void CheckAbortProgress() {
    if (g_abortStage == 0 || !g_loopObj) return;
    uint32_t now = NowMs();
    uint32_t loopState = 0;
    SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &loopState);

    if (g_abortStage == 1) {
        if ((int32_t)(now - g_abortAtMs) < 600) return;
        if (loopState == ID_MUSIC_PLAY) {
            Logf("quickskip: abort verify FAILED - MUSIC_LOOP still 0x23880000 "
                 "after 600 ms, using fallback (MUSIC_RESULT path)");
            if (g_setNextState) {
                void* obj = g_loopObj;
                FnSetNextState f = g_setNextState;
                GuardedCall("fallback SetNextState(MUSIC_LOOP,MUSIC_RESULT)",
                            [f, obj]() { f(obj, ID_MUSIC_RESULT); });
            }
            g_abortStage = 2;
            g_abortAtMs = now;
        } else {
            Logf("quickskip: abort verify OK - MUSIC_LOOP state now 0x%08X "
                 "(left MUSIC_PLAY)", loopState);
            g_abortStage = 0;
        }
    } else if (g_abortStage == 2) {
        if ((int32_t)(now - g_abortAtMs) < 1500) return;
        Logf("quickskip: fallback result state loop=0x%08X -> requesting "
             "junction 0x23830000", loopState);
        if (g_setNextState && loopState == ID_MUSIC_RESULT) {
            void* obj = g_loopObj;
            FnSetNextState f = g_setNextState;
            GuardedCall("fallback SetNextState(MUSIC_LOOP,MUSIC_JUNCTION)",
                        [f, obj]() { f(obj, ID_MUSIC_JUNCTION); });
        }
        g_abortStage = 3;
        g_abortAtMs = now;
    } else if (g_abortStage == 3) {
        if ((int32_t)(now - g_abortAtMs) < 2000) return;
        Logf("quickskip: fallback final state loop=0x%08X", loopState);
        g_abortStage = 0;
    }
}

// ---------------------------------------------------------------------------
//  abort sequence engine
//
//  A plain forced transition MUSIC_LOOP -> MUSIC_SELECT is not enough in this
//  build.  The play HUD (song info / "1ST TUNE" / LEVEL chip / START-END lane)
//  is drawn by shared AEP UI data sets; the game only lays them out for the
//  select screen while walking the native post-tune chain.  Jumping straight
//  to MUSIC_SELECT therefore leaves the play layout on top of the select
//  screen.  The mod instead walks a short prefix of the real chain:
//
//      MUSIC_RESULT 0x238A0000         (~30 frames / 1 s)
//        -> 0x238D0000 (continue page) (~30 frames / 1 s; does the AEP teardown)
//        -> junction  0x23830000       (native routing)
//        -> MUSIC_SELECT 0x23840000
//
//  AbortMode selects the variant:
//    15 = default: 30 + 30 frames, ~2.8 s from trigger to select
//    22 = faster : 20 + 15 frames, ~1.7 s
//    23 = fastest: 15 +  8 frames, ~1.3 s
//     1 = legacy : immediate MUSIC_SELECT (very fast but leaves the play HUD
//                  on the select screen in this build; debugging only)
//
//  Every game call below is wrapped in GuardedCall(); failures just fall back
//  to the plain abort (mode 1), so a mismatch can never leave the game stuck.
// ---------------------------------------------------------------------------
bool TransitObj(const char* what, void* obj, uint32_t state) {
    if (!g_transitState || !obj) return false;
    FnTransitState transit = g_transitState;
    int ret = 0;
    bool ok = GuardedCall(what, [transit, obj, state, &ret]() {
        ret = transit(obj, state);
    });
    Logf("quickskip: TransitState(%s, 0x%08X) called=%d ret=%d", what, state,
         ok ? 1 : 0, ret);
    return ok;
}

uint32_t ChainWaitResult() {
    return (g_seqMode == 22) ? 20u : (g_seqMode == 23) ? 15u : 30u;
}

uint32_t ChainWaitContinue() {
    return (g_seqMode == 22) ? 15u : (g_seqMode == 23) ? 8u : 30u;
}

void StartAbort(int mode) {
    g_seqMode = mode;
    g_seqStage = SEQ_NONE;
    g_seqFrames = 0;

    // Stop the music first.  The native result state would do this itself,
    // but we transition into it immediately, so do it here to avoid a frame
    // of overlaid play audio.
    bool stopped = false;
    if (g_bgmStop) {
        FnNoArg bgm = g_bgmStop;
        stopped = GuardedCall("sound_bgm_stop", [bgm]() { bgm(); });
    }
    if (g_cfg.stopSfx && g_seStopAll) {
        FnNoArg se = g_seStopAll;
        GuardedCall("sound_se_stop_all", [se]() { se(); });
    }

    uint32_t loopState = 0, playState = 0;
    if (g_loopObj) SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &loopState);
    if (g_playObj) SafeReadU32((uintptr_t)g_playObj + OBJ_STATE, &playState);
    Logf("quickskip: ABORT mode=%d bgmStopped=%d loopState=0x%08X "
         "playState=0x%08X", mode, stopped ? 1 : 0, loopState, playState);

    if (mode == 1) {
        TransitObj("MUSIC_LOOP->MUSIC_SELECT", g_loopObj, ID_MUSIC_SELECT);
        g_abortAtMs = NowMs();
        g_abortStage = 1;
        return;
    }

    // Default / 22 / 23: native post-tune chain.
    g_abortAtMs = NowMs();
    TransitObj("MUSIC_LOOP->MUSIC_RESULT", g_loopObj, ID_MUSIC_RESULT);
    g_seqStage = SEQ_WAIT_RESULT;
    g_seqAtMs = g_abortAtMs;
}

void ProgressAbortSequence() {
    if (g_seqStage == SEQ_NONE) return;
    uint32_t now = NowMs();
    uint32_t loopState = 0;
    if (g_loopObj) SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &loopState);

    if (g_seqStage == SEQ_WAIT_RESULT) {
        if ((int32_t)(now - g_seqAtMs) < 100) return;
        if (loopState != ID_MUSIC_RESULT) {
            // The router may still be finishing the previous transition; retry.
            // If it never accepts MUSIC_RESULT (different build), fall back to
            // the legacy direct jump so the tune is never left playing.
            if ((int32_t)(now - g_abortAtMs) > 4000) {
                Logf("quickskip: WARN MUSIC_RESULT timeout (loop=0x%08X), "
                     "falling back to direct MUSIC_SELECT", loopState);
                TransitObj("MUSIC_LOOP->MUSIC_SELECT", g_loopObj,
                           ID_MUSIC_SELECT);
                g_abortStage = 1;
                g_abortAtMs = now;
                g_seqStage = SEQ_NONE;
            } else {
                g_seqAtMs = now;
            }
            return;
        }
        Logf("quickskip: mode%d reached MUSIC_RESULT, waiting %u frames",
             g_seqMode, ChainWaitResult());
        g_seqStage = SEQ_CHAIN_C;
        g_seqFrames = 0;
        g_seqAtMs = now;
    } else if (g_seqStage == SEQ_CHAIN_C) {
        ++g_seqFrames;
        if (g_seqFrames < ChainWaitResult()) return;
        Logf("quickskip: mode%d result done -> continue page 0x238D0000 "
             "(loop=0x%08X, frames=%u)", g_seqMode, loopState, g_seqFrames);
        TransitObj("MUSIC_LOOP->CONTINUE_SELECT(0x238D0000)", g_loopObj,
                   ID_CONTINUE_SELECT);
        g_seqStage = SEQ_CHAIN_D;
        g_seqFrames = 0;
        g_seqAtMs = now;
    } else if (g_seqStage == SEQ_CHAIN_D) {
        ++g_seqFrames;
        if (g_seqFrames < ChainWaitContinue()) return;
        Logf("quickskip: mode%d continue done -> junction (loop=0x%08X, "
             "frames=%u)", g_seqMode, loopState, g_seqFrames);
        TransitObj("MUSIC_LOOP->MUSIC_JUNCTION", g_loopObj, ID_MUSIC_JUNCTION);
        g_abortAtMs = NowMs();
        g_abortStage = 1;
        g_seqStage = SEQ_NONE;
    }
}

// ---------------------------------------------------------------------------
//  input.dll!input_update detour - runs once per game frame, game thread
// ---------------------------------------------------------------------------
void __cdecl InputUpdateDetour() {
    if (g_inputUpdateOrig) {
        FnInputUpdate orig = g_inputUpdateOrig;
        GuardedCall("input_update(original)", [orig]() { orig(); });
    }
    if (!g_cfg.enabled || g_disabled || !g_ready) return;

    if (!g_objectsReady) {
        ValidateObjectsDeferred();
        if (!g_objectsReady) return;
    }

    // Remote/debug trigger requests (export QuickSkipTriggerNow) are executed
    // here, on the game thread, between two frames.
    if (InterlockedExchange(&g_pendingAbort, 0) != 0) {
        LONG ovr = InterlockedExchange(&g_modeOverride, 0);
        bool inPlayNow = InSongPlay();
        Logf("quickskip: remote trigger picked up (inPlay=%d modeOverride=%ld)",
             inPlayNow ? 1 : 0, (long)ovr);
        if (inPlayNow) {
            int mode = (ovr > 0 && ovr <= 20) ? (int)ovr : g_cfg.abortMode;
            StartAbort(mode);
        }
    }

    uint32_t now = NowMs();
    CheckAbortProgress();
    ProgressAbortSequence();
    bool inPlay = InSongPlay();

    if (inPlay) {
        g_everPlayed = true;
        if (TriggerHeld()) {
            if (g_heldSinceMs == 0) g_heldSinceMs = now;
            uint32_t heldFor = now - g_heldSinceMs;
            bool cooldownOk =
                (g_lastAbortMs == 0) ||
                ((int32_t)(now - g_lastAbortMs) >= g_cfg.cooldownMs);
            if (!g_holdConsumed && heldFor >= (uint32_t)g_cfg.holdMs &&
                cooldownOk) {
                g_holdConsumed = true;
                g_lastAbortMs = now;
                uint32_t mask = 0, loopState = 0, playState = 0;
                ReadPanelMask(&mask);
                SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &loopState);
                SafeReadU32((uintptr_t)g_playObj + OBJ_STATE, &playState);
                Logf("quickskip: TRIGGER keys held %lu ms mask=0x%04X "
                     "need=0x%04X panels=[%s] loop=0x%08X play=0x%08X "
                     "-> abort tune",
                     (unsigned long)heldFor, mask, g_cfg.keysMask,
                     DecodePanels(mask).c_str(), loopState, playState);
                StartAbort(g_cfg.abortMode);
            }
        } else {
            g_heldSinceMs = 0;
            g_holdConsumed = false;
        }
    } else {
        // Not playing: reset trigger state so a fresh hold is required.
        if (g_heldSinceMs != 0 || g_holdConsumed) {
            if (g_cfg.verbose) {
                Logf("quickskip: left play state, trigger reset");
            }
        }
        g_heldSinceMs = 0;
        g_holdConsumed = false;
    }
}

// ---------------------------------------------------------------------------
//  init
// ---------------------------------------------------------------------------
void* ResolveExport(uintptr_t base, const char* name, uint32_t rvaFallback) {
    if (!base) return nullptr;
    void* p = (void*)GetProcAddress((HMODULE)base, name);
    if (!p && rvaFallback) p = (void*)(base + rvaFallback);
    return p;
}

// Returns 1 = validated, 0 = play loop not entered yet (retry), -1 = mismatch.
//
// Only the MUSIC_LOOP router object is required here: it gets its id when the
// game enters the play loop (song select).  The MUSIC_PLAY scene object is
// created later, when a tune actually starts, and is re-created for every
// tune, so it is checked lazily inside InSongPlay() instead.
int ValidateObjects() {
    g_loopObj = (void*)(g_jubeat + RVA_OBJ_MUSIC_LOOP);
    g_playObj = (void*)(g_jubeat + RVA_OBJ_MUSIC_PLAY);
    uint32_t loopObjId = 0;
    if (!SafeReadU32((uintptr_t)g_loopObj + OBJ_ID, &loopObjId)) {
        Logf("quickskip: cannot read MUSIC_LOOP object at %p", g_loopObj);
        return -1;
    }
    if (loopObjId == 0) return 0;  // play loop not entered yet
    if (loopObjId != ID_MUSIC_LOOP) {
        Logf("quickskip: ERROR MUSIC_LOOP object id mismatch (got 0x%08X "
             "expected 0x%08X)", loopObjId, ID_MUSIC_LOOP);
        return -1;
    }
    char loopName[64] = {};
    uint32_t lp = 0;
    SafeReadU32((uintptr_t)g_loopObj + OBJ_NAME_PTR, &lp);
    if (lp) SafeReadCString(lp, loopName, sizeof(loopName));
    uint32_t playObjId = 0;
    SafeReadU32((uintptr_t)g_playObj + OBJ_ID, &playObjId);
    Logf("quickskip: FSM MUSIC_LOOP ok: obj=%p id=0x%08X name=\"%s\" "
         "(MUSIC_PLAY obj=%p id=0x%08X attaches when a tune starts)",
         g_loopObj, loopObjId, loopName, g_playObj, playObjId);
    return 1;
}

bool TryInit() {
    if (g_ready || g_disabled) return g_ready;

    HMODULE jb = (HMODULE)GetModuleHandleA("jubeat.dll");
    HMODULE gf = (HMODULE)GetModuleHandleA("gftools.dll");
    HMODULE sd = (HMODULE)GetModuleHandleA("sound.dll");
    HMODULE in = (HMODULE)GetModuleHandleA("input.dll");
    if (!jb || !gf || !sd || !in) {
        uint32_t now = NowMs();
        if (g_lastModuleTryMs == 0 || (now - g_lastModuleTryMs) >= 5000) {
            g_lastModuleTryMs = now;
            Logf("quickskip: waiting for game modules (jubeat=%p gftools=%p "
                 "sound=%p input=%p)", (void*)jb, (void*)gf, (void*)sd,
                 (void*)in);
        }
        return false;
    }

    g_jubeat = (uintptr_t)jb;
    g_gftools = (uintptr_t)gf;
    g_sound = (uintptr_t)sd;

    DWORD ts = 0;
    if (!ReadLoadedPeTimeStamp(jb, &ts)) {
        Logf("quickskip: ERROR cannot read jubeat.dll PE header, disabling");
        g_disabled = true;
        return false;
    }
    uintptr_t jbBase = 0;
    size_t jbSize = 0;
    GetModuleRange(jb, &jbBase, &jbSize);
    Logf("quickskip: jubeat.dll base=%p size=0x%zX timestamp=0x%08X "
         "expected=0x%08X", (void*)jbBase, jbSize, (unsigned)ts,
         (unsigned)kExpectedTimeStamp);
    if (ts != kExpectedTimeStamp) {
        Logf("quickskip: ERROR version mismatch - this mod supports jubeat.dll "
             "timestamp 0x%08X only. Disabling (game unaffected).",
             (unsigned)kExpectedTimeStamp);
        g_disabled = true;
        return false;
    }

    g_getStateMap = (FnGetStateMap)ResolveExport(g_gftools,
        "GFInputDDevGetStateMap", 0x1CA70);
    g_transitState = (FnTransitState)ResolveExport(g_gftools,
        "GFFSMObjectTransitState", 0x6560);
    g_setNextState = (FnSetNextState)ResolveExport(g_gftools,
        "GFFSMObjectSetNextState", 0x6520);
    g_bgmStop = (FnNoArg)ResolveExport(g_sound, "sound_bgm_stop", 0);
    g_seStopAll = (FnNoArg)ResolveExport(g_sound, "sound_se_stop_all", 0);
    g_inputUpdateOrig = nullptr;

    Logf("quickskip: funcs getStateMap=%p transitState=%p bgmStop=%p "
         "seStopAll=%p", (void*)g_getStateMap, (void*)g_transitState,
         (void*)g_bgmStop, (void*)g_seStopAll);
    if (!g_getStateMap || !g_transitState || !g_bgmStop) {
        Logf("quickskip: ERROR required exports missing, disabling");
        g_disabled = true;
        return false;
    }
    void* target = (void*)GetProcAddress(in, "input_update");
    if (!target) target = (void*)((uintptr_t)in + 0x1040);
    Logf("quickskip: input_update target resolved via %s (%p)",
         target == (void*)GetProcAddress(in, "input_update") ? "export" : "RVA",
         target);
    // sanity check of the expected input.dll prologue (push ebp; mov ebp,esp)
    const uint8_t* pro = (const uint8_t*)target;
    if (!IsReadableRange((uintptr_t)pro, 8) || pro[0] != 0x55 ||
        pro[1] != 0x8B || pro[2] != 0xEC) {
        Logf("quickskip: ERROR unexpected input_update prologue at %p, "
             "disabling", target);
        g_disabled = true;
        return false;
    }
    void* trampoline = nullptr;
    if (!HookInstall(target, (void*)InputUpdateDetour, &trampoline)) {
        Logf("quickskip: ERROR input_update hook failed, disabling");
        g_disabled = true;
        return false;
    }
    g_inputUpdateOrig = (FnInputUpdate)trampoline;
    g_ready = true;
    Logf("quickskip: READY - input_update hooked at %p (trampoline %p) "
         "keys=0x%04X panels=[%s] holdMs=%d. Hold those panels during play "
         "to return to song select.", target, trampoline, g_cfg.keysMask,
         DecodePanels(g_cfg.keysMask).c_str(), g_cfg.holdMs);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  feature interface (called by the harness worker thread)
// ---------------------------------------------------------------------------
void OnDllReady() {
    EnsureGuard();
    LoadConfig();
    Logf("quickskip: initialising (pid=%lu)", (unsigned long)GetCurrentProcessId());
    for (int i = 0; i < 600 && !g_ready && !g_disabled; ++i) {  // up to 60 s
        if (TryInit()) break;
        Sleep(100);
    }
    if (!g_ready && !g_disabled) {
        Logf("quickskip: ERROR game modules did not appear in time, disabling");
        g_disabled = true;
    }
    if (g_ready) {
        Logf("quickskip: hooks installed; play detection will activate once "
             "the game reaches the play loop (song select)");
    }
}

void OnTick(DWORD uptimeMs) {
    if (!g_ready || g_disabled) return;
    static DWORD last = 0;
    if (uptimeMs - last >= 10000) {
        last = uptimeMs;
        Logf("quickskip: alive (up=%lu s) heldSince=%lu lastAbort=%lu played=%d",
             (unsigned long)(uptimeMs / 1000), (unsigned long)g_heldSinceMs,
             (unsigned long)g_lastAbortMs, g_everPlayed ? 1 : 0);
    }
}

void OnHotkey() {
    // Keyboard hotkey is not used by this mod (trigger is the four corner
    // panels inside the game). Kept for harness compatibility.
}

void OnShutdown() {
    Logf("quickskip: shutting down hooks");
    g_ready = false;
}

}  // namespace quickskip

// ---------------------------------------------------------------------------
//  public helpers used by the exported test entry points (see bottom)
// ---------------------------------------------------------------------------
namespace quickskip {
void SetModeOverride(int mode) { InterlockedExchange(&g_modeOverride, (LONG)mode); }
void RequestAbort() {
    InterlockedExchange(&g_pendingAbort, 1);
}
bool IsReady() { return g_ready && !g_disabled; }
bool IsInPlay() { return InSongPlay(); }
uint32_t CurrentLoopState() {
    uint32_t st = 0;
    if (g_loopObj) SafeReadU32((uintptr_t)g_loopObj + OBJ_STATE, &st);
    return st;
}
uint32_t CurrentPlayState() {
    uint32_t st = 0;
    if (g_playObj) SafeReadU32((uintptr_t)g_playObj + OBJ_STATE, &st);
    return st;
}
}  // namespace quickskip

// Exported debug/remote trigger: executed on the next game frame.
extern "C" __declspec(dllexport) DWORD WINAPI QuickSkipTriggerNow(DWORD arg) {
    harness::Logf("quickskip: export QuickSkipTriggerNow(arg=%lu) requested",
                  (unsigned long)arg);
    if (arg >= 1 && arg <= 23) {
        quickskip::SetModeOverride((int)arg);
    }
    quickskip::RequestAbort();
    return 1;
}

// Exported status query: returns a packed status (ready | inPlay | states).
extern "C" __declspec(dllexport) DWORD WINAPI QuickSkipStatus(DWORD) {
    DWORD v = 0;
    if (quickskip::IsReady()) v |= 1;
    if (quickskip::IsInPlay()) v |= 2;
    harness::Logf("quickskip: status ready=%d inPlay=%d loop=0x%08X play=0x%08X",
                  quickskip::IsReady() ? 1 : 0, quickskip::IsInPlay() ? 1 : 0,
                  quickskip::CurrentLoopState(), quickskip::CurrentPlayState());
    return v;
}

namespace feature {
void OnDllReady() { quickskip::OnDllReady(); }
void OnTick(DWORD uptimeMs) { quickskip::OnTick(uptimeMs); }
void OnHotkey() { quickskip::OnHotkey(); }
void OnShutdown() { quickskip::OnShutdown(); }
}  // namespace feature
