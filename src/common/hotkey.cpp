// ---------------------------------------------------------------------------
//  hotkey.cpp - "all keys held" hotkey poller.
//
//  For the real jubeat feature the four corner panels (1, 4, 13, 16) should be
//  read from the game's own input state (input.dll / jubeat.dll), but as a
//  build plumbing proof this skeleton polls GetAsyncKeyState for a set of
//  virtual key codes.  Keys are configurable via the HOOK_HOTKEYS environment
//  variable (comma separated decimal VK codes), default "1 2 3 4".
// ---------------------------------------------------------------------------
#include "common.h"

namespace harness {
namespace {

int  g_keys[kMaxHotkeys] = {0x31, 0x32, 0x33, 0x34}; // '1' '2' '3' '4'
int  g_count = 4;
bool g_prevAll = false;

} // namespace

void HotkeyConfigure(const int* vks, int count) {
    if (count < 0) count = 0;
    if (count > kMaxHotkeys) count = kMaxHotkeys;
    for (int i = 0; i < count; ++i) g_keys[i] = vks[i];
    g_count = count;
    g_prevAll = false;
}

int HotkeyCount() { return g_count; }

bool HotkeyAllHeldRisingEdge() {
    bool all = g_count > 0;
    for (int i = 0; i < g_count; ++i) {
        if (!(GetAsyncKeyState(g_keys[i]) & 0x8000)) {
            all = false;
            break;
        }
    }
    bool edge = all && !g_prevAll;
    g_prevAll = all;
    return edge;
}

} // namespace harness
