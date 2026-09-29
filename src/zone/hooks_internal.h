// hooks_internal.h - Shared state for the transition, readiness and camera zone hooks.
// Transition stamps can be published off-main; reporting and priority flags are main-thread work.

#ifndef KENSHI_ZONE_OPT_HOOKS_INTERNAL_H
#define KENSHI_ZONE_OPT_HOOKS_INTERNAL_H

#include "base/core.h"

namespace hooks_detail
{ // namespace hooks_detail

extern bool prioritizedThisTransition;
extern volatile LONG g_tgtLogPending;
void LogTransitionTarget();
void FlushPendingTransitionStart();
void CountBracketFrame(void* zm);
void ReadinessReportTick(double now);

} // namespace hooks_detail


#endif // KENSHI_ZONE_OPT_HOOKS_INTERNAL_H
