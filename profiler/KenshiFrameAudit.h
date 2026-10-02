// KenshiFrameAudit - per-frame time attribution for KEOProfiler.
//
// Splits every frame into named sections that sum to the frame time (Ogre
// render window, the frame listeners, each call inside GameWorld::mainLoop),
// measures the wake/run/join of the AI, birds and physics worker threads,
// samples the world state that drives cost, and writes percentile summaries,
// [SLOW] lines and a per-second CSV under <dll dir>\audit\. All file I/O runs
// on a reporter thread. Steam 1.0.65 only; configured by KEOProfiler.ini.

#pragma once

#include <string>
#include <stdint.h>

// First thing in startPlugin: records the main thread, reads the INI, starts
// the reporter thread (which also writes KEOProfiler.log).
void Audit_Init(uintptr_t gameBase, const std::string& dllDir);

// Last thing in startPlugin: installs the audit hooks and call-site probes
// (unless [Audit] Enabled=0) and queues the install report.
void Audit_Install();

bool Audit_IsMainThread();
bool Audit_LegacyFpsEnabled();

// Queues one finished line for KEOProfiler.log. Safe on any thread.
void Audit_LogProfiler(const std::string& line);

// The profiler wrote (and DebugLogged) its own lines during this frame: the
// frame is kept out of the percentiles so the tool doesn't measure itself.
void Audit_MarkProfilerLogFrame();

// Called by the profiler's existing hooks. Main thread unless noted.
void Audit_MainLoopEnter(float time);
void Audit_MainLoopExit();
void Audit_MeshesEnter();
void Audit_MeshesExit();
void Audit_StateMachineMs(double ms);
void Audit_SquadActivated();
void Audit_TransitionEdge(bool on);    // any thread
