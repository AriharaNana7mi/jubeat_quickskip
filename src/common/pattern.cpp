// ---------------------------------------------------------------------------
//  pattern.cpp - byte + wildcard pattern scanner.
//
//  Pattern syntax (IDA style):
//     "48 8B 05 ?? ?? ?? ?? E8"       '??' = any byte
//     "4? 8? ?? E8"                   nibble wildcards
//  Scans PE section ranges of a loaded module (so we never touch guard pages),
//  executable sections only by default.
// ---------------------------------------------------------------------------
#include "common.h"

namespace harness {
namespace {

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool MatchAt(const uint8_t* p, const BytePattern& pat) {
    for (size_t i = 0; i < pat.length; ++i) {
        if ((p[i] & pat.mask[i]) != pat.value[i]) return false;
    }
    return true;
}

uintptr_t ScanModuleInternal(HMODULE mod, const BytePattern& pat,
                             bool executableOnly) {
    if (!pat.valid || pat.length == 0) return 0;
    uintptr_t base = 0;
    size_t imageSize = 0;
    if (!GetModuleRange(mod, &base, &imageSize)) return 0;

    const uint8_t* p = (const uint8_t*)base;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)p;
    const IMAGE_NT_HEADERS32* nt =
        (const IMAGE_NT_HEADERS32*)(p + dos->e_lfanew);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt) + i;
        if (executableOnly && !(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        if (!(sec->Characteristics & (IMAGE_SCN_MEM_READ)))
            continue;
        if (sec->SizeOfRawData == 0 && sec->Misc.VirtualSize == 0) continue;
        size_t va = sec->VirtualAddress;
        size_t sz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize
                                          : sec->SizeOfRawData;
        if (va >= imageSize) continue;
        if (sz > imageSize - va) sz = imageSize - va;
        uintptr_t hit = ScanRange(base + va, sz, pat);
        if (hit) return hit;
    }
    return 0;
}

} // namespace

bool ParsePattern(const char* idaStyle, BytePattern* out) {
    if (!idaStyle || !out) return false;
    out->value.clear();
    out->mask.clear();
    out->length = 0;
    out->valid = false;

    const char* p = idaStyle;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' ||
               *p == '\n')
            ++p;
        if (!*p) break;

        char tok[4] = {0, 0, 0, 0};
        int count = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\r' &&
               *p != '\n') {
            if (count >= 3) return false; // token too long
            tok[count++] = *p++;
        }

        uint8_t value = 0, mask = 0;
        if (count == 1) {
            if (tok[0] != '?') {
                int n = HexNibble(tok[0]);
                if (n < 0) return false;
                value = (uint8_t)(n << 4);
                mask = 0xF0;
            }
        } else if (count == 2) {
            for (int i = 0; i < 2; ++i) {
                if (tok[i] == '?') continue;
                int n = HexNibble(tok[i]);
                if (n < 0) return false;
                if (i == 0) {
                    value |= (uint8_t)(n << 4);
                    mask |= 0xF0;
                } else {
                    value |= (uint8_t)n;
                    mask |= 0x0F;
                }
            }
        } else {
            return false;
        }
        out->value.push_back(value);
        out->mask.push_back(mask);
    }
    out->length = out->value.size();
    out->valid = out->length > 0;
    return out->valid;
}

uintptr_t ScanRange(uintptr_t start, size_t size, const BytePattern& pat) {
    if (!start || !pat.valid || pat.length == 0) return 0;
    if (size < pat.length) return 0;
    const uint8_t* p = (const uint8_t*)start;
    size_t last = size - pat.length;
    for (size_t i = 0; i <= last; ++i) {
        if (pat.mask[0] == 0xFF && p[i] != pat.value[0]) continue;
        if (MatchAt(p + i, pat)) return start + i;
    }
    return 0;
}

uintptr_t ScanModule(HMODULE mod, const char* idaStyle, bool executableOnly) {
    BytePattern pat;
    if (!ParsePattern(idaStyle, &pat)) return 0;
    return ScanModuleInternal(mod, pat, executableOnly);
}

uintptr_t ScanModuleAny(HMODULE mod, const char* idaStyle) {
    BytePattern pat;
    if (!ParsePattern(idaStyle, &pat)) return 0;
    return ScanModuleInternal(mod, pat, false);
}

} // namespace harness
