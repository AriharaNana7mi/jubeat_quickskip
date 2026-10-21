// ---------------------------------------------------------------------------
//  feature.h - the ONLY place the game specific feature logic lives.
//
//  The Lead drops the "restart current tune" implementation into
//  feature_stub.cpp (or a replacement .cpp implementing this interface).
//  The harness plumbing never calls game code directly, it only calls these
//  four callbacks from the worker thread.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

namespace feature {

// Called once after the DLL has initialised, modules were logged and
// jubeat.dll has (or has not) been waited for.  Do pattern scans here.
void OnDllReady();

// Called ~every 20-40 ms by the worker thread.  uptimeMs counts from worker
// start.  Use it for state machines / timeouts.
void OnTick(DWORD uptimeMs);

// Called on the rising edge of "all four corner panels held".
// In the final version this triggers "restart current tune".
void OnHotkey();

// Called right before the worker thread exits / DLL unloads.
void OnShutdown();

} // namespace feature
