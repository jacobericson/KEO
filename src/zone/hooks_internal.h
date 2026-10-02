// hooks_internal.h - Shared state for the transition, readiness and camera zone hooks.
// Transition stamps can be published off-main; reporting and priority flags are main-thread work.

#ifndef KEO_HOOKS_INTERNAL_H
#define KEO_HOOKS_INTERNAL_H

#include "base/core.h"

namespace hooks_detail
{ // namespace hooks_detail

extern bool prioritizedThisTransition;
void FlushPendingTransitionLines();
void CountBracketFrame(void* zm);
void ReadinessReportTick(double now);

} // namespace hooks_detail


#endif // KEO_HOOKS_INTERNAL_H
