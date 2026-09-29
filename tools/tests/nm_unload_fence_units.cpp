// The fence's order is pure; these fakes record the operations it calls.
#include <cstring>
#include "navmesh/jobs/nm_unload_fence_policy.h"
#include "check.h"

struct FakeFence
{
	char trace[32];
	int beginAnswer;
	int pjAnswer;
	bool bumpJob, bumpClaim;
	long job, claim;
};

static void Note(FakeFence* f, char c)
{
	size_t n = strlen(f->trace);
	f->trace[n] = c;
	f->trace[n + 1] = 0;
}
static int  Begin(void* p)      { FakeFence* f = (FakeFence*)p; Note(f, 'b'); if (f->bumpJob) ++f->job; if (f->bumpClaim) ++f->claim; return f->beginAnswer; }
static long SkipJob(void* p)    { Note((FakeFence*)p, 'j'); return ((FakeFence*)p)->job; }
static long SkipClaim(void* p)  { Note((FakeFence*)p, 'c'); return ((FakeFence*)p)->claim; }
static int  TryPj(void* p)      { Note((FakeFence*)p, 'p'); return ((FakeFence*)p)->pjAnswer; }
static void UnlockPj(void* p)   { Note((FakeFence*)p, 'u'); }
static void End(void* p)        { Note((FakeFence*)p, 'e'); }
static void Priority(void* p)   { Note((FakeFence*)p, 'r'); }

static NmFenceResult Run(FakeFence* f, int beginAnswer, int pjAnswer, bool bumpJob, bool bumpClaim)
{
	memset(f, 0, sizeof(*f));
	f->beginAnswer = beginAnswer;
	f->pjAnswer = pjAnswer;
	f->bumpJob = bumpJob;
	f->bumpClaim = bumpClaim;
	NmFenceOps ops = { f, &Begin, &SkipJob, &SkipClaim, &TryPj, &UnlockPj, &End, &Priority };
	return NmFenceTryBegin(ops);
}

static void Release(FakeFence* f, NmFenceResult* r)
{
	NmFenceOps ops = { f, &Begin, &SkipJob, &SkipClaim, &TryPj, &UnlockPj, &End, &Priority };
	NmFenceRelease(r, ops);
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
	return CheckExit("nm_unload_fence_units");
}
