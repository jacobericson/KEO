// The worker pool decision through the shipped policy, over its three flags.

#include <cstdio>
#include <cstring>
#include "navmesh/workers/nm_worker_gate_policy.h"

#include "check.h"

int main()
{
	Check(NmPoolDecide(true, true, true) == NMPOOL_CREATE,
	      "decide: the install finished and both hooks are in: create");
	Check(NmPoolDecide(true, false, true) == NMPOOL_REFUSE_STOPHOOK,
	      "decide: NavMesh::stop is not hooked: refuse, stopHook");
	Check(NmPoolDecide(true, true, false) == NMPOOL_REFUSE_BUILDHOOK,
	      "decide: buildCollision is not hooked: refuse, buildHook");
	Check(NmPoolDecide(true, false, false) == NMPOOL_REFUSE_STOPHOOK,
	      "decide: neither hook: stopHook is the reported cause");
	Check(NmPoolDecide(false, true, true) == NMPOOL_WAIT,
	      "decide: the install has not finished: wait");
	Check(NmPoolDecide(false, false, true) == NMPOOL_WAIT,
	      "decide: an unfinished install never refuses early (stop flag 0)");
	Check(NmPoolDecide(false, true, false) == NMPOOL_WAIT,
	      "decide: an unfinished install never refuses early (build flag 0)");
	Check(NmPoolDecide(false, false, false) == NMPOOL_WAIT,
	      "decide: an unfinished install never refuses early (both flags 0)");
	const char* ts = NmPoolRefusalToken(NMPOOL_REFUSE_STOPHOOK);
	Check(ts != NULL && strcmp(ts, "refused(stopHook)") == 0,
	      "token: a stop-hook refusal reads refused(stopHook)");
	const char* tb = NmPoolRefusalToken(NMPOOL_REFUSE_BUILDHOOK);
	Check(tb != NULL && strcmp(tb, "refused(buildHook)") == 0,
	      "token: a build-hook refusal reads refused(buildHook)");
	Check(NmPoolRefusalToken(NMPOOL_CREATE) == NULL, "token: a created pool has none");
	Check(NmPoolRefusalToken(NMPOOL_WAIT) == NULL, "token: an undecided pool has none");
	return CheckExit("nm_worker_gate_units");
}
