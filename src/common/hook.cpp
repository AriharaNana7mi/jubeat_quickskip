// ---------------------------------------------------------------------------
//  hook.cpp - minimal x86 (32-bit) inline hook engine.
//
//  Strategy
//  --------
//  * decode the first >= 5 bytes of the target function with a hand rolled
//    length decoder (common 32-bit prologues are fully covered),
//  * allocate a trampoline within +/-2GB of the target, copy the stolen bytes,
//    re-encode their relative branches (target inside the stolen block is
//    remapped into the trampoline, target outside is recomputed),
//  * append "jmp target+stolen" to the trampoline,
//  * optionally suspend every other thread of the process while the 5-byte
//    E9 rel32 is written, so a thread can never observe a half patched
//    instruction,
//  * keep the original bytes for a version safe uninstall (checked by the
//    test harness: hook, fire, unhook, original behaviour restored).
//
//  Limits: no relocation of RIP-relative code (not applicable on x86), no
//  support for prologues that cannot be safely split within 32 bytes, 32-bit
//  only.  If a prologue cannot be decoded the install fails cleanly and the
//  caller may fall back to IatHook().
// ---------------------------------------------------------------------------
#include "common.h"

#include <cstdint>

namespace harness {
namespace {

constexpr int kMaxHooks  = 24;
constexpr int kMaxStolen = 32;
constexpr int kMaxRetired = 64;

struct Reloc {
    size_t instrStart;  // offset from hook target
    size_t instrLen;
    size_t relOffset;   // offset of displacement inside the instruction
    int    relSize;     // 1 or 4
};

struct InlineHookEntry {
    void*   target     = nullptr;
    void*   detour     = nullptr;
    uint8_t* trampoline = nullptr;
    uint8_t saved[kMaxStolen] = {};
    size_t  stolen     = 0;
    bool    active     = false;
};

INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_cs;
InlineHookEntry  g_hooks[kMaxHooks];

// Trampolines are never freed after a successful unhook: another thread may
// have been suspended while executing inside the trampoline (or be about to
// return from the original prologue).  Freeing it would turn that thread into
// a use-after-free crash.  Old trampolines are retired (memory kept until
// process exit); a few dozen bytes each is a cheap price for a safe teardown.
uint8_t* g_retired[kMaxRetired];
int      g_retiredCount = 0;

void RetireTrampoline(uint8_t* p) {
    if (!p) return;
    if (g_retiredCount < kMaxRetired) g_retired[g_retiredCount++] = p;
    // else: intentionally leaked
}

BOOL CALLBACK InitOnceCb(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    return TRUE;
}
void EnsureCs() { InitOnceExecuteOnce(&g_once, InitOnceCb, nullptr, nullptr); }

struct Insn {
    size_t len = 0;
    int    relKind = 0; // 0 none, 1 rel8, 2 rel32
    size_t relOffset = 0;
};

bool DecodeModRm32(const uint8_t* p, size_t max, size_t* outLen) {
    if (max < 1) return false;
    uint8_t modrm = p[0];
    uint8_t mod = modrm >> 6;
    uint8_t rm  = modrm & 7;
    size_t i = 1;
    if (mod == 3) {
        *outLen = i;
        return true;
    }
    if (rm == 4) { // SIB
        if (i >= max) return false;
        uint8_t sib = p[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    } else if (mod == 0 && rm == 5) {
        i += 4;
    }
    if (mod == 1)
        i += 1;
    else if (mod == 2)
        i += 4;
    if (i > max) return false;
    *outLen = i;
    return true;
}

// Returns false for unknown / non-decodable encodings (caller aborts install).
bool DecodeOne(const uint8_t* p, size_t max, Insn* out) {
    size_t i = 0;
    bool opsize = false;
    bool addrsize = false;
    for (;;) {
        if (i >= max) return false;
        uint8_t b = p[i];
        if (b == 0x66) { opsize = true; ++i; continue; }
        if (b == 0x67) { addrsize = true; ++i; continue; }
        if (b == 0xF0 || b == 0xF2 || b == 0xF3) { ++i; continue; }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 ||
            b == 0x65) {
            ++i;
            continue;
        }
        break;
    }
    if (i >= max) return false;

    uint8_t op = p[i++];
    size_t imm = 0;
    int relKind = 0;
    size_t relOff = 0;
    bool hasModRm = false;

    auto fits = [&](size_t extra) { return i + extra <= max; };

    if (op == 0x0F) {
        if (!fits(1)) return false;
        uint8_t op2 = p[i++];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            relKind = 2;
            relOff = i;
            imm = 4;
        } else if (op2 == 0x38) {
            if (!fits(1)) return false;
            ++i;
            hasModRm = true;
        } else if (op2 == 0x3A) {
            if (!fits(1)) return false;
            ++i;
            hasModRm = true;
            imm = 1;
        } else if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 ||
                   op2 == 0xA4 || op2 == 0xAC || op2 == 0xBA || op2 == 0xC2 ||
                   op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6) {
            hasModRm = true;
            imm = 1;
        } else if (op2 == 0x05 || op2 == 0x06 || op2 == 0x07 || op2 == 0x08 ||
                   op2 == 0x09 || op2 == 0x0B || op2 == 0x0E || op2 == 0x30 ||
                   op2 == 0x31 || op2 == 0x32 || op2 == 0x33 || op2 == 0x34 ||
                   op2 == 0x35 || op2 == 0x37 || op2 == 0x77 || op2 == 0xA2 ||
                   op2 == 0xAA || op2 == 0xB9) {
            // no operands
        } else {
            hasModRm = true;
        }
    } else if (op < 0x40) {
        uint8_t low = op & 7;
        if (op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 ||
            op == 0x17 || op == 0x1E || op == 0x1F || op == 0x27 ||
            op == 0x2F || op == 0x37 || op == 0x3F) {
            // no operands
        } else if (low <= 3) {
            hasModRm = true;
        } else if (low == 4) {
            imm = 1;
        } else if (low == 5) {
            imm = opsize ? 2 : 4;
        } else {
            return false;
        }
    } else if (op <= 0x5F) {
        // inc/dec/push/pop reg: no operands
    } else if (op == 0x60 || op == 0x61) {
        // pusha/popa
    } else if (op == 0x62 || op == 0x63) {
        hasModRm = true;
    } else if (op == 0x68) {
        imm = opsize ? 2 : 4;
    } else if (op == 0x69) {
        hasModRm = true;
        imm = opsize ? 2 : 4;
    } else if (op == 0x6A) {
        imm = 1;
    } else if (op == 0x6B) {
        hasModRm = true;
        imm = 1;
    } else if (op >= 0x6C && op <= 0x6F) {
        // ins/outs
    } else if (op >= 0x70 && op <= 0x7F) {
        relKind = 1;
        relOff = i;
        imm = 1;
    } else if (op == 0x80 || op == 0x82 || op == 0x83) {
        hasModRm = true;
        imm = 1;
    } else if (op == 0x81) {
        hasModRm = true;
        imm = opsize ? 2 : 4;
    } else if (op >= 0x84 && op <= 0x8F) {
        hasModRm = true;
    } else if (op >= 0x90 && op <= 0x99) {
        // nop/xchg/cbw/cwd
    } else if (op == 0x9A) {
        imm = 6; // ptr16:32
    } else if (op >= 0x9B && op <= 0x9F) {
        // wait/pushf/popf/sahf/lahf
    } else if (op >= 0xA0 && op <= 0xA3) {
        imm = addrsize ? 2 : 4; // moffs
    } else if (op >= 0xA4 && op <= 0xA7) {
        // movs/cmps
    } else if (op == 0xA8) {
        imm = 1;
    } else if (op == 0xA9) {
        imm = opsize ? 2 : 4;
    } else if (op >= 0xAA && op <= 0xAF) {
        // stos/lods/scas
    } else if (op >= 0xB0 && op <= 0xB7) {
        imm = 1;
    } else if (op >= 0xB8 && op <= 0xBF) {
        imm = opsize ? 2 : 4;
    } else if (op == 0xC0 || op == 0xC1) {
        hasModRm = true;
        imm = 1;
    } else if (op == 0xC2) {
        imm = 2;
    } else if (op == 0xC3) {
        // ret
    } else if (op == 0xC4 || op == 0xC5) {
        hasModRm = true;
    } else if (op == 0xC6) {
        hasModRm = true;
        imm = 1;
    } else if (op == 0xC7) {
        hasModRm = true;
        imm = opsize ? 2 : 4;
    } else if (op == 0xC8) {
        imm = 3; // enter imm16, imm8
    } else if (op == 0xC9) {
        // leave
    } else if (op == 0xCA) {
        imm = 2;
    } else if (op == 0xCB || op == 0xCC) {
        // retf/int3
    } else if (op == 0xCD) {
        imm = 1;
    } else if (op == 0xCE || op == 0xCF) {
        // into/iret
    } else if (op >= 0xD0 && op <= 0xD3) {
        hasModRm = true;
    } else if (op == 0xD4 || op == 0xD5) {
        imm = 1;
    } else if (op == 0xD6 || op == 0xD7) {
        // salc/xlat
    } else if (op >= 0xD8 && op <= 0xDF) {
        hasModRm = true; // x87
    } else if (op >= 0xE0 && op <= 0xE3) {
        relKind = 1;
        relOff = i;
        imm = 1;
    } else if (op >= 0xE4 && op <= 0xE7) {
        imm = 1;
    } else if (op == 0xE8 || op == 0xE9) {
        relKind = 2;
        relOff = i;
        imm = 4;
    } else if (op == 0xEA) {
        imm = 6; // ptr16:32
    } else if (op == 0xEB) {
        relKind = 1;
        relOff = i;
        imm = 1;
    } else if (op >= 0xEC && op <= 0xEF) {
        // in/out
    } else if (op == 0xF1) {
        // int1
    } else if (op == 0xF4 || op == 0xF5) {
        // hlt/cmc
    } else if (op == 0xF6 || op == 0xF7) {
        hasModRm = true;
        // imm present only for /0 and /1; resolved after the ModRM decode
    } else if (op >= 0xF8 && op <= 0xFD) {
        // clc/stc/.../std
    } else if (op == 0xFE || op == 0xFF) {
        hasModRm = true;
    } else {
        return false;
    }

    if (hasModRm) {
        size_t modrmPos = i;
        size_t mlen = 0;
        if (!DecodeModRm32(p + i, max - i, &mlen)) return false;
        i += mlen;
        if (op == 0xF6 || op == 0xF7) {
            uint8_t reg = (uint8_t)((p[modrmPos] >> 3) & 7);
            if (reg <= 1) imm = (op == 0xF6) ? 1 : (opsize ? 2 : 4);
        }
    }
    if (imm) {
        if (!fits(imm)) return false;
        i += imm;
    }
    out->len = i;
    out->relKind = relKind;
    out->relOffset = relOff;
    return true;
}

// ---------------------------------------------------------------------------
//  Thread suspension helpers (best effort; if snapshot fails we still patch
//  under the assumption that a 5-byte aligned-ish write is atomic on x86,
//  which is NOT guaranteed - that is why the caller can request suspension).
// ---------------------------------------------------------------------------
struct ThreadBatch {
    HANDLE handles[1024];
    int    count = 0;
};

void SuspendOtherThreads(ThreadBatch* batch) {
    batch->count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    DWORD me = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) {
                CloseHandle(h);
                continue;
            }
            if (batch->count < 1024)
                batch->handles[batch->count++] = h;
            else
                CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void ResumeOtherThreads(ThreadBatch* batch) {
    for (int i = 0; i < batch->count; ++i) {
        ResumeThread(batch->handles[i]);
        CloseHandle(batch->handles[i]);
    }
    batch->count = 0;
}

bool FitsRel32(const void* from, const void* to) {
    int64_t d = (int64_t)(intptr_t)to - (int64_t)(intptr_t)from;
    return d >= INT32_MIN && d <= INT32_MAX;
}

// Allocate executable memory within +/-2GB of `near` so a 5-byte rel jmp can
// reach it.  Plain VirtualAlloc first (usually enough on 32-bit), then a
// hint scan.
uint8_t* AllocNear(uintptr_t nearAddr, size_t size) {
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p) {
        if (FitsRel32((void*)nearAddr, p) && FitsRel32(p, (void*)nearAddr))
            return (uint8_t*)p;
        VirtualFree(p, 0, MEM_RELEASE);
    }
    const uintptr_t gran = 0x10000;
    uintptr_t base = nearAddr & ~(gran - 1);
    for (int dir = 0; dir < 2; ++dir) {
        for (uintptr_t delta = gran; delta <= 0x30000000ULL; delta += gran) {
            uintptr_t cand = dir ? base + delta : base - delta;
            if (cand < gran) continue;
            p = VirtualAlloc((void*)cand, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
            if (!p) continue;
            if (FitsRel32((void*)nearAddr, p) && FitsRel32(p, (void*)nearAddr))
                return (uint8_t*)p;
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

} // namespace

size_t HookDecodeLength(const uint8_t* code, size_t maxLen) {
    Insn ins;
    if (!code || maxLen == 0 || !DecodeOne(code, maxLen, &ins)) return 0;
    return ins.len;
}

bool HookInstall(void* target, void* detour, void** trampoline,
                 bool suspendOtherThreads) {
    if (!target || !detour) return false;
    EnsureCs();
    EnterCriticalSection(&g_cs);

    InlineHookEntry* slot = nullptr;
    for (auto& h : g_hooks) {
        if (h.active && h.target == target) {
            LeaveCriticalSection(&g_cs);
            return false; // already hooked
        }
        if (!h.active && !slot) slot = &h;
    }
    if (!slot) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    const uint8_t* code = (const uint8_t*)target;
    size_t stolen = 0;
    Reloc relocs[16];
    int nrel = 0;
    bool decodable = true;
    while (stolen < 5) {
        Insn ins;
        if (stolen >= kMaxStolen ||
            !DecodeOne(code + stolen, kMaxStolen - stolen, &ins)) {
            decodable = false;
            break;
        }
        if (ins.relKind) {
            if (nrel >= 16) {
                decodable = false;
                break;
            }
            Reloc r;
            r.instrStart = stolen;
            r.instrLen = ins.len;
            r.relOffset = ins.relOffset;
            r.relSize = (ins.relKind == 1) ? 1 : 4;
            relocs[nrel++] = r;
        }
        stolen += ins.len;
    }
    if (!decodable || stolen < 5 || stolen > kMaxStolen) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    uint8_t* tramp = AllocNear((uintptr_t)target, stolen + 16);
    if (!tramp) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    memcpy(tramp, code, stolen);
    bool relok = true;
    for (int r = 0; r < nrel && relok; ++r) {
        const Reloc& rr = relocs[r];
        size_t dispPos = rr.instrStart + rr.relOffset;
        int64_t origDisp;
        if (rr.relSize == 1)
            origDisp = (int64_t)(int8_t)tramp[dispPos];
        else {
            int32_t v;
            memcpy(&v, tramp + dispPos, 4);
            origDisp = v;
        }
        int64_t origInstrEnd =
            (int64_t)(uintptr_t)code + rr.instrStart + rr.instrLen;
        int64_t branchTarget = origInstrEnd + origDisp;
        int64_t newTarget = branchTarget;
        // Branch inside the stolen block: remap into the trampoline.
        if (branchTarget >= (int64_t)(uintptr_t)code &&
            branchTarget < (int64_t)(uintptr_t)(code + stolen)) {
            newTarget = (int64_t)(uintptr_t)tramp +
                        (branchTarget - (int64_t)(uintptr_t)code);
        }
        int64_t newEnd = (int64_t)(uintptr_t)tramp + rr.instrStart + rr.instrLen;
        int64_t newDisp = newTarget - newEnd;
        if (rr.relSize == 1) {
            if (newDisp < -128 || newDisp > 127) {
                relok = false;
                break;
            }
            tramp[dispPos] = (uint8_t)(int8_t)newDisp;
        } else {
            if (newDisp < INT32_MIN || newDisp > INT32_MAX) {
                relok = false;
                break;
            }
            int32_t d = (int32_t)newDisp;
            memcpy(tramp + dispPos, &d, 4);
        }
    }
    if (!relok) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        LeaveCriticalSection(&g_cs);
        return false;
    }

    // trampoline: stolen bytes + jmp back to target+stolen
    size_t backOff = stolen;
    tramp[backOff] = 0xE9;
    int64_t backDisp = (int64_t)(uintptr_t)(code + stolen) -
                       ((int64_t)(uintptr_t)tramp + backOff + 5);
    if (backDisp < INT32_MIN || backDisp > INT32_MAX) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        LeaveCriticalSection(&g_cs);
        return false;
    }
    int32_t bd = (int32_t)backDisp;
    memcpy(tramp + backOff + 1, &bd, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, stolen + 5);

    int64_t jmpDisp = (int64_t)(uintptr_t)detour - ((int64_t)(uintptr_t)target + 5);
    if (jmpDisp < INT32_MIN || jmpDisp > INT32_MAX) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        LeaveCriticalSection(&g_cs);
        return false;
    }

    ThreadBatch batch;
    if (suspendOtherThreads) SuspendOtherThreads(&batch);

    DWORD oldProt = 0;
    bool patched = false;
    if (VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(slot->saved, target, stolen);
        uint8_t patch[5];
        patch[0] = 0xE9;
        int32_t d = (int32_t)jmpDisp;
        memcpy(patch + 1, &d, 4);
        memcpy(target, patch, 5);
        FlushInstructionCache(GetCurrentProcess(), target, stolen);
        DWORD tmp = 0;
        VirtualProtect(target, stolen, oldProt, &tmp);
        patched = true;
    }

    if (suspendOtherThreads) ResumeOtherThreads(&batch);

    if (!patched) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        LeaveCriticalSection(&g_cs);
        return false;
    }

    slot->target = target;
    slot->detour = detour;
    slot->trampoline = tramp;
    slot->stolen = stolen;
    slot->active = true;
    if (trampoline) *trampoline = tramp;
    LeaveCriticalSection(&g_cs);
    return true;
}

bool HookRemove(void* target) {
    if (!target) return false;
    EnsureCs();
    EnterCriticalSection(&g_cs);
    InlineHookEntry* slot = nullptr;
    for (auto& h : g_hooks) {
        if (h.active && h.target == target) {
            slot = &h;
            break;
        }
    }
    if (!slot) {
        LeaveCriticalSection(&g_cs);
        return false;
    }

    ThreadBatch batch;
    SuspendOtherThreads(&batch);
    DWORD oldProt = 0;
    bool restored = false;
    if (VirtualProtect(target, slot->stolen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(target, slot->saved, slot->stolen);
        FlushInstructionCache(GetCurrentProcess(), target, slot->stolen);
        DWORD tmp = 0;
        VirtualProtect(target, slot->stolen, oldProt, &tmp);
        restored = true;
    }
    ResumeOtherThreads(&batch);

    if (slot->trampoline) RetireTrampoline(slot->trampoline);
    slot->target = nullptr;
    slot->detour = nullptr;
    slot->trampoline = nullptr;
    slot->stolen = 0;
    slot->active = false;
    LeaveCriticalSection(&g_cs);
    return restored;
}

void HookRemoveAll() {
    EnsureCs();
    void* targets[kMaxHooks];
    int count = 0;
    EnterCriticalSection(&g_cs);
    for (auto& h : g_hooks)
        if (h.active) targets[count++] = h.target;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < count; ++i) HookRemove(targets[i]);
}

} // namespace harness
