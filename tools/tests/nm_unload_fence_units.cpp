// The fence's order is pure; these fakes record the operations it calls.
#include <cstring>
#include "navmesh/jobs/nm_unload_fence_policy.h"
#include "check.h"

struct FakeFence
{
	char trace[32];
	int beginAnswer;
	int pjAnswer;
	int mode;
	bool bumpJob, bumpClaim;
	long job, claim;
};

static void Note(FakeFence* f, char c)
{
	size_t n = strlen(f->trace);
	f->trace[n] = c;
	f->trace[n + 1] = 0;
}
static int  Begin(void* p, int mode) { FakeFence* f = (FakeFence*)p; Note(f, 'b'); f->mode = mode; if (f->bumpJob) ++f->job; if (f->bumpClaim) ++f->claim; return f->beginAnswer; }
static long SkipJob(void* p)    { Note((FakeFence*)p, 'j'); return ((FakeFence*)p)->job; }
static long SkipClaim(void* p)  { Note((FakeFence*)p, 'c'); return ((FakeFence*)p)->claim; }
static int  TryPj(void* p)      { Note((FakeFence*)p, 'p'); return ((FakeFence*)p)->pjAnswer; }
static void UnlockPj(void* p)   { Note((FakeFence*)p, 'u'); }
static void End(void* p)        { Note((FakeFence*)p, 'e'); }
static void Priority(void* p)   { Note((FakeFence*)p, 'r'); }

static NmFenceOps Ops(FakeFence* f)
{
	NmFenceOps ops = { f, &Begin, &SkipJob, &SkipClaim, &TryPj, &UnlockPj, &End, &Priority };
	return ops;
}

static NmFenceResult Run(FakeFence* f, int beginAnswer, int pjAnswer, bool bumpJob, bool bumpClaim)
{
	memset(f, 0, sizeof(*f));
	f->mode = -1;
	f->beginAnswer = beginAnswer;
	f->pjAnswer = pjAnswer;
	f->bumpJob = bumpJob;
	f->bumpClaim = bumpClaim;
	NmFenceResult rec = NM_FENCE_RELEASED;
	return NmFenceTryBegin(&rec, Ops(f));
}

static void Release(FakeFence* f, NmFenceResult* r)
{
	NmFenceRelease(r, Ops(f));
}

// The claims mode with both counts set to move on a begin: it must read neither.
static NmFenceResult RunClaims(FakeFence* f, int beginAnswer)
{
	memset(f, 0, sizeof(*f));
	f->mode = -1;
	f->beginAnswer = beginAnswer;
	f->pjAnswer = NM_FENCE_PJ_HELD;
	f->bumpJob = true;
	f->bumpClaim = true;
	NmFenceResult rec = NM_FENCE_RELEASED;
	return NmFenceTryBegin(&rec, Ops(f), NM_FENCE_MODE_CLAIMS);
}

static void TestClaimsMode(FakeFence* f)
{
	NmFenceResult r = RunClaims(f, NM_FENCE_BEGIN_OK);
	Check(r == NM_FENCE_CLAIMS_ONLY && strcmp(f->trace, "b") == 0 && f->mode == NM_FENCE_MODE_CLAIMS,
	      "claims: begun with one operation, no processJobCS try");
	Check(NmFenceProceeds(r), "claims: a begun fence proceeds");
	Release(f, &r);
	Check(strcmp(f->trace, "be") == 0 && r == NM_FENCE_RELEASED, "claims: the release ends the publication, no unlock");
	Release(f, &r);
	Check(strcmp(f->trace, "be") == 0, "claims: a second release does nothing");

	r = RunClaims(f, NM_FENCE_BEGIN_CLAIMED);
	Check(r == NM_FENCE_REFUSED_CLAIM && strcmp(f->trace, "b") == 0, "claims: a claim's refusal is named by the begin, no count read");
	r = RunClaims(f, NM_FENCE_BEGIN_IDLE);
	Check(r == NM_FENCE_IDLE && strcmp(f->trace, "b") == 0 && !NmFenceProceeds(r), "claims: no generator is idle and does not proceed");
	r = RunClaims(f, NM_FENCE_BEGIN_UNAVAILABLE);
	Check(r == NM_FENCE_UNAVAILABLE && strcmp(f->trace, "b") == 0, "claims: unavailable");
	r = RunClaims(f, NM_FENCE_BEGIN_REFUSED);
	Check(r == NM_FENCE_REFUSED && strcmp(f->trace, "b") == 0, "claims: another refusal");

	static const int kAnswers[5] = { NM_FENCE_BEGIN_REFUSED, NM_FENCE_BEGIN_OK, NM_FENCE_BEGIN_UNAVAILABLE,
	                                 NM_FENCE_BEGIN_IDLE, NM_FENCE_BEGIN_CLAIMED };
	bool quiet = true;
	for (int i = 0; i < 5; ++i)
	{
		r = RunClaims(f, kAnswers[i]);
		Release(f, &r);
		quiet = quiet && r == NM_FENCE_RELEASED && !strchr(f->trace, 'p') && !strchr(f->trace, 'u') &&
		        !strchr(f->trace, 'r') && !strchr(f->trace, 'j') && !strchr(f->trace, 'c');
	}
	Check(quiet, "claims: no answer tries, unlocks or prioritises processJobCS, or reads a count");

	NmFenceResult held = RunClaims(f, NM_FENCE_BEGIN_OK);
	NmFenceResult again = NmFenceTryBegin(&held, Ops(f), NM_FENCE_MODE_CLAIMS);
	Check(again == NM_FENCE_REFUSED && held == NM_FENCE_CLAIMS_ONLY && strcmp(f->trace, "b") == 0,
	      "claims: a second begin refuses and keeps the publication owed");
	Release(f, &held);
	Check(strcmp(f->trace, "be") == 0, "claims: the kept publication ends exactly once");

	r = Run(f, NM_FENCE_BEGIN_OK, NM_FENCE_PJ_HELD, false, false);
	Check(f->mode == NM_FENCE_MODE_FULL && strcmp(f->trace, "jcbp") == 0,
	      "full: the begin is asked for the full mode and the trace is unchanged");
	Release(f, &r);
	r = Run(f, NM_FENCE_BEGIN_IDLE, NM_FENCE_PJ_HELD, false, false);
	Check(r == NM_FENCE_REFUSED && strcmp(f->trace, "jcbjc") == 0, "full: an idle answer is a plain refusal");
	r = Run(f, NM_FENCE_BEGIN_CLAIMED, NM_FENCE_PJ_HELD, false, false);
	Check(r == NM_FENCE_REFUSED && strcmp(f->trace, "jcbjc") == 0, "full: a claimed answer without its count is a plain refusal");

	Check(NM_FENCE_CLAIMS_ONLY == 8 && NM_FENCE_IDLE == 9 && NmFenceProceeds(NM_FENCE_CLAIMS_ONLY) && !NmFenceProceeds(NM_FENCE_IDLE),
	      "the claims mode's values follow released; only its begun fence proceeds");
}

int main()
{
	FakeFence f;
	NmFenceResult r = Run(&f, NM_FENCE_BEGIN_UNAVAILABLE, NM_FENCE_PJ_HELD, false, false);
	Check(r == NM_FENCE_UNAVAILABLE && strcmp(f.trace, "jcb") == 0, "unavailable: counts read, begun, nothing else");
	r = Run(&f, NM_FENCE_BEGIN_REFUSED, NM_FENCE_PJ_HELD, true, false);
	Check(r == NM_FENCE_REFUSED_JOB && strcmp(f.trace, "jcbj") == 0, "a queued job's refusal is named by its count");
	r = Run(&f, NM_FENCE_BEGIN_REFUSED, NM_FENCE_PJ_HELD, false, true);
	Check(r == NM_FENCE_REFUSED_CLAIM && strcmp(f.trace, "jcbjc") == 0, "a claim's refusal is named by its count");
	r = Run(&f, NM_FENCE_BEGIN_REFUSED, NM_FENCE_PJ_HELD, false, false);
	Check(r == NM_FENCE_REFUSED && strcmp(f.trace, "jcbjc") == 0, "another refusal is named neither");
	r = Run(&f, NM_FENCE_BEGIN_OK, NM_FENCE_PJ_TIMEOUT, false, false);
	Check(r == NM_FENCE_DEFER_PJ && strcmp(f.trace, "jcbper") == 0, "a busy processJobCS ends the publication, then asks for priority");
	Release(&f, &r);
	Check(strcmp(f.trace, "jcbper") == 0 && r == NM_FENCE_RELEASED, "a deferral owes no release");
	r = Run(&f, NM_FENCE_BEGIN_OK, NM_FENCE_PJ_HELD, false, false);
	Check(r == NM_FENCE_HELD && strcmp(f.trace, "jcbp") == 0, "held: begun and locked");
	Release(&f, &r);
	Check(strcmp(f.trace, "jcbpue") == 0, "held: processJobCS released before the publication");
	Release(&f, &r);
	Check(strcmp(f.trace, "jcbpue") == 0, "a second release does nothing");
	r = Run(&f, NM_FENCE_BEGIN_OK, NM_FENCE_PJ_NONE, false, false);
	Check(r == NM_FENCE_NO_LOCK && strcmp(f.trace, "jcbp") == 0, "no lock: begun, nothing locked");
	Release(&f, &r);
	Check(strcmp(f.trace, "jcbpe") == 0, "no lock: only the publication ends");

	static const bool kProceeds[NM_FENCE_RELEASED + 1] = { true, true, false, false, false, false, false, false };
	bool proceedsOk = NM_FENCE_RELEASED == 7;
	for (int v = 0; v <= NM_FENCE_RELEASED; ++v)
		proceedsOk = proceedsOk && NmFenceProceeds((NmFenceResult)v) == kProceeds[v];
	Check(proceedsOk, "only a begun fence proceeds");

	struct RefusalCase { int begin; bool bumpJob, bumpClaim; NmFenceResult want; const char* trace; };
	static const RefusalCase kRefusals[4] =
	{
		{ NM_FENCE_BEGIN_UNAVAILABLE, false, false, NM_FENCE_UNAVAILABLE,   "jcb" },
		{ NM_FENCE_BEGIN_REFUSED,     true,  false, NM_FENCE_REFUSED_JOB,   "jcbj" },
		{ NM_FENCE_BEGIN_REFUSED,     false, true,  NM_FENCE_REFUSED_CLAIM, "jcbjc" },
		{ NM_FENCE_BEGIN_REFUSED,     false, false, NM_FENCE_REFUSED,       "jcbjc" }
	};
	bool refusalsOk = true;
	for (int i = 0; i < 4; ++i)
	{
		r = Run(&f, kRefusals[i].begin, NM_FENCE_PJ_HELD, kRefusals[i].bumpJob, kRefusals[i].bumpClaim);
		refusalsOk = refusalsOk && r == kRefusals[i].want;
		Release(&f, &r);
		refusalsOk = refusalsOk && strcmp(f.trace, kRefusals[i].trace) == 0 && r == NM_FENCE_RELEASED;
	}
	Check(refusalsOk, "a refusal owes no release: the trace stays the begin's and the record reads released");

	NmFenceResult held = Run(&f, NM_FENCE_BEGIN_OK, NM_FENCE_PJ_HELD, false, false);
	NmFenceResult again = NmFenceTryBegin(&held, Ops(&f));
	Check(again == NM_FENCE_REFUSED && held == NM_FENCE_HELD && strcmp(f.trace, "jcbp") == 0,
	      "a second begin on a held fence refuses before any operation and keeps the hold");
	Release(&f, &held);
	again = NmFenceTryBegin(&held, Ops(&f));
	Check(strcmp(f.trace, "jcbpuejcbp") == 0 && again == NM_FENCE_HELD && held == NM_FENCE_HELD,
	      "the kept hold releases once, and the released fence begins again");
	TestClaimsMode(&f);
	return CheckExit("nm_unload_fence_units");
}
