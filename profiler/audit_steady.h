// audit_steady.h - What the rest of the frame audit calls in the per-thread CPU sampler
// (audit_cpu.cpp) and the steady-state cost probes (audit_steady.cpp). Included after
// audit_detail.h by the audit's own sources.

#ifndef KENSHI_FRAME_AUDIT_STEADY_H
#define KENSHI_FRAME_AUDIT_STEADY_H

#include "audit_detail.h"

namespace kenshiframeaudit_detail {

// ---- Per-thread CPU time ----
enum CpuRole { CPU_ROLE_AI, CPU_ROLE_PHYS, CPU_ROLE_BIRDS, CPU_ROLE_COUNT };
// A worker body's own thread records its role at entry. Interlocked only.
void CpuNoteRole(int role);
// An Ogre worker records itself when it reaches Barrier::sync. Interlocked only.
void CpuNoteOgreWorker();
// Reporter thread, every pass: one sample a second once the TSC rate is known.
void CpuSampleTick();

// ---- Steady-state cost probes (SteadyDetail) ----
// A call-site row that only SteadyDetail installs.
bool SteadySiteTag(int tag);
// Startup, from InstallExeHooks: the six entry hooks.
void InstallSteadyHooks();
// "on", "partial" or "off"; meaningful once InstallSites has run.
const char* SteadyStatus();
// Main thread, in CollectAi once the AI slot is consistent.
void SteadyCollectAi(const ThreadSlot& s);
// Main thread, in CloseFrame's in-game block: the detail metrics and counts into the record.
void SteadyFrameTotals(const CurFrame& c, FrameRec& r);

} // kenshiframeaudit_detail

#endif
