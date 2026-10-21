// ---------------------------------------------------------------------------
//  probe_api.h - public interface of the probe feature used by worker.cpp
//  (kept tiny so the worker does not need to know probe internals).
// ---------------------------------------------------------------------------
#pragma once

namespace feature {

// minimal declaration set is in feature.h; this header exists so the probe
// build has an obvious include point.  feature::OnDllReady / OnTick /
// OnHotkey / OnShutdown are implemented by src/probe/feature_probe.cpp.
}
