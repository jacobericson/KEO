// transition_hook.h - Transition hook and shared main-thread priority entry.

#ifndef KENSHI_ZONE_OPT_TRANSITION_HOOK_H
#define KENSHI_ZONE_OPT_TRANSITION_HOOK_H

void hook_showLoadingMessage(void* thisPtr, bool on);

namespace hooks_detail
{ // namespace hooks_detail

void CallPrioritizeNavMeshQueue();

} // namespace hooks_detail
using namespace ::hooks_detail;


#endif // KENSHI_ZONE_OPT_TRANSITION_HOOK_H
