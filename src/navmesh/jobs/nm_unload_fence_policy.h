// nm_unload_fence_policy.h - The mod-unload fence's order over injected operations. No game or platform types.
#ifndef KENSHI_ZONE_OPT_NM_UNLOAD_FENCE_POLICY_H
#define KENSHI_ZONE_OPT_NM_UNLOAD_FENCE_POLICY_H

enum NmFenceBegin { NM_FENCE_BEGIN_REFUSED = 0, NM_FENCE_BEGIN_OK, NM_FENCE_BEGIN_UNAVAILABLE };
enum NmFencePj { NM_FENCE_PJ_NONE = 0, NM_FENCE_PJ_HELD, NM_FENCE_PJ_TIMEOUT };

enum NmFenceResult
{
	NM_FENCE_HELD = 0,        // begun, processJobCS held: the unload may run
	NM_FENCE_NO_LOCK,         // begun, processJobCS never initialised: the unload may run
	NM_FENCE_REFUSED_JOB,     // a job for the zone is queued
	NM_FENCE_REFUSED_CLAIM,   // a claim for the zone is in flight
	NM_FENCE_REFUSED,         // refused for another transient reason
	NM_FENCE_DEFER_PJ,        // processJobCS busy: the publication is ended and the priority asked
	NM_FENCE_UNAVAILABLE,     // can never pass in this build or session
	NM_FENCE_RELEASED         // nothing is owed
};

struct NmFenceOps
{
	void* ctx;
	int  (*begin)(void* ctx);              // NmFenceBegin
	long (*skipJobCount)(void* ctx);
	long (*skipClaimCount)(void* ctx);
	int  (*tryPj)(void* ctx);              // NmFencePj, one try with no wait
	void (*unlockPj)(void* ctx);
	void (*end)(void* ctx);
	void (*requestPriority)(void* ctx);
};

// Reads both refusal counts, begins, and on a refusal names it by the count
// the begin raised. Once begun, tries processJobCS once; when it is busy,
// ends the publication and asks for priority before returning.
NmFenceResult NmFenceTryBegin(const NmFenceOps& ops);
// For a result that may run: processJobCS first when held, then the
// publication. *r becomes NM_FENCE_RELEASED, so a second call releases nothing.
void NmFenceRelease(NmFenceResult* r, const NmFenceOps& ops);

inline bool NmFenceProceeds(NmFenceResult r) { return r == NM_FENCE_HELD || r == NM_FENCE_NO_LOCK; }

#endif
