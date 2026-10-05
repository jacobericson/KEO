// nm_unload_fence_policy.h - The mod-unload fence's order over injected operations. No game or platform types.
#ifndef KEO_NM_UNLOAD_FENCE_POLICY_H
#define KEO_NM_UNLOAD_FENCE_POLICY_H

// FULL refuses a zone with a queued job or a claim and takes processJobCS;
// CLAIMS refuses only a claim and takes nothing but the publication.
enum NmFenceMode { NM_FENCE_MODE_FULL = 0, NM_FENCE_MODE_CLAIMS };

// IDLE and CLAIMED are answers of the claims mode only.
enum NmFenceBegin { NM_FENCE_BEGIN_REFUSED = 0, NM_FENCE_BEGIN_OK, NM_FENCE_BEGIN_UNAVAILABLE,
                    NM_FENCE_BEGIN_IDLE, NM_FENCE_BEGIN_CLAIMED };
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
	NM_FENCE_RELEASED,        // nothing is owed
	NM_FENCE_CLAIMS_ONLY,     // claims mode, begun: the unload may run; only the publication is owed
	NM_FENCE_IDLE             // claims mode: no claim can exist yet, and nothing was published
};

struct NmFenceOps
{
	void* ctx;
	int  (*begin)(void* ctx, int mode);    // NmFenceBegin, for an NmFenceMode
	long (*skipJobCount)(void* ctx);
	long (*skipClaimCount)(void* ctx);
	int  (*tryPj)(void* ctx);              // NmFencePj, one try with no wait
	void (*unlockPj)(void* ctx);
	void (*end)(void* ctx);
	void (*requestPriority)(void* ctx);
};

// Full mode: reads both refusal counts, begins, and on a refusal names it by
// the count the begin raised. Once begun, tries processJobCS once; when it is
// busy, ends the publication and asks for priority before returning. Claims
// mode: begins and nothing else, and names a refusal by the begin's answer.
// *r is the fence's record: unless it reads NM_FENCE_RELEASED, the call
// returns NM_FENCE_REFUSED before any operation and leaves *r as it was, so a
// second begin never drops a hold the first still owes. Otherwise *r becomes
// the result.
NmFenceResult NmFenceTryBegin(NmFenceResult* r, const NmFenceOps& ops, NmFenceMode mode = NM_FENCE_MODE_FULL);
// For a result that may run: processJobCS first when held, then the
// publication. *r becomes NM_FENCE_RELEASED, so a second call releases nothing.
void NmFenceRelease(NmFenceResult* r, const NmFenceOps& ops);

inline bool NmFenceProceeds(NmFenceResult r)
{
	return r == NM_FENCE_HELD || r == NM_FENCE_NO_LOCK || r == NM_FENCE_CLAIMS_ONLY;
}

#endif
