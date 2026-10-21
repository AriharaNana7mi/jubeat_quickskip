// ---------------------------------------------------------------------------
//  worker.h - background worker thread of hook_dll.dll.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

namespace harness {
namespace worker {

bool Start(HMODULE self);
void RequestStop();
bool Join(DWORD timeoutMs);
bool StopRequested();

} // namespace worker
} // namespace harness
