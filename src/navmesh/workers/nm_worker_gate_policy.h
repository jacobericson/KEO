#ifndef KEO_NM_WORKER_GATE_POLICY_H
#define KEO_NM_WORKER_GATE_POLICY_H

// Whether the navmesh worker pool may start. It needs two hooks the
// first-dispatch install puts in: NavMesh::stop, whose hook retires the
// workers before the game frees the Havok heap, and buildCollision, whose
// hook covers the collision builds the workers run beside the bg thread. The
// pool starts only once both are known to be installed. Pure: no game header.

enum NmPoolDecision
{
	NMPOOL_WAIT = 0,            // the first-dispatch install has not finished: decide later
	NMPOOL_CREATE,              // start the workers
	NMPOOL_REFUSE_STOPHOOK,     // NavMesh::stop is not hooked: no pool this session
	NMPOOL_REFUSE_BUILDHOOK     // buildCollision is not hooked: no pool this session
};

// lazyInstallDone: every first-dispatch hook's outcome is known.
// stopHookInstalled: NavMesh::stop is hooked. buildHookInstalled:
// buildCollision is hooked. With both hooks missing the answer is
// NMPOOL_REFUSE_STOPHOOK.
NmPoolDecision NmPoolDecide(bool lazyInstallDone, bool stopHookInstalled, bool buildHookInstalled);

// The workers= value of a refusal ("refused(stopHook)" or
// "refused(buildHook)"); NULL otherwise.
const char* NmPoolRefusalToken(NmPoolDecision decision);

#endif
