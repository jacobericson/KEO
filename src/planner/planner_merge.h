// planner_merge.h - What the formation reads of a merged run-together order: its gather point and the
// members the merge left alone, and the re-plan once the group has gathered. Main thread only; every
// entry returns at once unless the planner is on.
#pragma once
#include <stdint.h>

namespace planner {

// From CreateFormationGroup with the order's characters, the leader first: true when the order capture
// merged this order in on (its anchor is chars[0], within a second); then gather receives the gather
// point and alone[i] is 1 for each chars[i] the merge left to walk its own route. False otherwise, with
// nothing written.
bool PlannerMergeGather(const uintptr_t* chars, int n, float gather[3], unsigned char* alone);
// From the formation's gather completion, before the travel send: each of chars[0..n) holding a plan
// whose hold the gather set is re-planned from where it stands, the hold cleared. One memo for the call,
// so a group standing on one node costs one search.
void PlannerResumeFromGather(const uintptr_t* chars, int n, double now);

} // namespace planner
