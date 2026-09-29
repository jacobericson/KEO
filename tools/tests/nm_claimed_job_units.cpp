// The policy and operation runner are pure; route scripts name engine
// checkpoints this suite cannot reach.
#include <cstdio>
#include <cstring>
#include "navmesh/jobs/nm_claimed_job_policy.h"
#include "check.h"

struct Recorder
{
	char trace[96];
	bool grants[2];
	int grantIndex;
};

static void Push(Recorder* r, const char* event)
{
	if (r->trace[0]) strcat_s(r->trace, sizeof(r->trace), ",");
	strcat_s(r->trace, sizeof(r->trace), event);
}
static void Write(void* p) { Push((Recorder*)p, "W"); }
static bool Begin(void* p)
{
	Recorder* r = (Recorder*)p;
	bool granted = r->grants[r->grantIndex++];
	Push(r, granted ? "B+" : "B-");
	return granted;
}
static void FreeClone(void* p) { Push((Recorder*)p, "F"); }
static void End(void* p) { Push((Recorder*)p, "E"); }
static void ClearSlot(void* p) { Push((Recorder*)p, "S"); }
static void Finished(void* p) { Push((Recorder*)p, "A"); }
static void Dropped(void* p) { Push((Recorder*)p, "D"); }
static void LeaveBusy(void* p) { Push((Recorder*)p, "L"); }
static void Idle(void* p) { Push((Recorder*)p, "I"); }

static CjReleaseOps Ops(Recorder* r)
{
	CjReleaseOps ops = { r, &Write, &Begin, &FreeClone, &End,
	                     &ClearSlot, &Finished, &Dropped, &LeaveBusy, &Idle };
	return ops;
}

static bool PlanEquals(const CjReleasePlan& a, const CjReleasePlan& b)
{
	return a.writePending == b.writePending && a.releaseClone == b.releaseClone
	    && a.tail == b.tail && a.bgAdj == b.bgAdj;
}

static void CheckPolicyMatrix()
{
	// Literal expectations for every point x (pending bit 0, clone bit 1,
	// original BG adjacency snapshot bit 2). No expectation uses policy code.
	const CjReleasePlan want[CJ_RELEASE_POINT_COUNT][8] = {
		{
			{false,false,CJ_TAIL_NONE,false}, {true,false,CJ_TAIL_NONE,false},
			{false,false,CJ_TAIL_NONE,false}, {true,false,CJ_TAIL_NONE,false},
			{false,false,CJ_TAIL_NONE,false}, {true,false,CJ_TAIL_NONE,false},
			{false,false,CJ_TAIL_NONE,false}, {true,false,CJ_TAIL_NONE,false}
		},
		{
			{false,false,CJ_TAIL_NONE,false}, {false,false,CJ_TAIL_NONE,false},
			{false,true,CJ_TAIL_NONE,false}, {false,true,CJ_TAIL_NONE,false},
			{false,false,CJ_TAIL_NONE,false}, {false,false,CJ_TAIL_NONE,false},
			{false,true,CJ_TAIL_NONE,false}, {false,true,CJ_TAIL_NONE,false}
		},
		{
			{false,false,CJ_TAIL_WORKER,false}, {false,false,CJ_TAIL_WORKER,false},
			{false,false,CJ_TAIL_WORKER,false}, {false,false,CJ_TAIL_WORKER,false},
			{false,false,CJ_TAIL_WORKER,false}, {false,false,CJ_TAIL_WORKER,false},
			{false,false,CJ_TAIL_WORKER,false}, {false,false,CJ_TAIL_WORKER,false}
		},
		{
			{false,false,CJ_TAIL_BG_DROP,false}, {false,false,CJ_TAIL_BG_DROP,false},
			{false,false,CJ_TAIL_BG_DROP,false}, {false,false,CJ_TAIL_BG_DROP,false},
			{false,false,CJ_TAIL_BG_DROP,true}, {false,false,CJ_TAIL_BG_DROP,true},
			{false,false,CJ_TAIL_BG_DROP,true}, {false,false,CJ_TAIL_BG_DROP,true}
		},
		{
			{false,false,CJ_TAIL_BG_FINISHED,false}, {false,false,CJ_TAIL_BG_FINISHED,false},
			{false,false,CJ_TAIL_BG_FINISHED,false}, {false,false,CJ_TAIL_BG_FINISHED,false},
			{false,false,CJ_TAIL_BG_FINISHED,true}, {false,false,CJ_TAIL_BG_FINISHED,true},
			{false,false,CJ_TAIL_BG_FINISHED,true}, {false,false,CJ_TAIL_BG_FINISHED,true}
		}
	};
	for (int point = 0; point < CJ_RELEASE_POINT_COUNT; ++point)
	{
		for (int bits = 0; bits < 8; ++bits)
		{
			CjReleaseFacts facts = { (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0 };
			CjReleasePlan got = CjReleasePlanFor((CjReleasePoint)point, facts);
			char label[96];
			sprintf_s(label, sizeof(label), "policy point=%d facts=%d: all four outputs and irrelevant-fact independence", point, bits);
			Check(PlanEquals(got, want[point][bits]), label);
		}
	}
}

static void CheckRoute(const char* label, const CjReleasePoint* points, int count,
                       CjReleaseFacts facts, bool firstGrant, bool secondGrant,
                       const char* expected)
{
	Recorder r = { "", { firstGrant, secondGrant }, 0 };
	CjReleaseOps ops = Ops(&r);
	for (int i = 0; i < count; ++i)
		CjRunReleases(CjReleasePlanFor(points[i], facts), ops);
	if (std::strcmp(r.trace, expected) != 0)
		std::printf("trace %s: got [%s], expected [%s]\n", label, r.trace, expected);
	Check(std::strcmp(r.trace, expected) == 0, label);
}

// A claimed job finished twice through CjFinishRun releases once: its trace is
// the one-finish route's, with one S and one L where the route has them.
static void CheckFinishTwice(const char* label, CjReleasePoint point, CjReleaseFacts facts, const char* once)
{
	Recorder r = { "", { true, true }, 0 };
	int stage = CJ_STAGE_CLAIMED;
	CjFinishRun(&stage, CjReleasePlanFor(point, facts), Ops(&r));
	CjFinishRun(&stage, CjReleasePlanFor(point, facts), Ops(&r));
	if (std::strcmp(r.trace, once) != 0)
		std::printf("trace %s: got [%s], expected [%s]\n", label, r.trace, once);
	Check(std::strcmp(r.trace, once) == 0, label);
}

static void CheckFinishOnce()
{
	int stage = CJ_STAGE_CLAIMED;
	Check(CjFinishAdmit(&stage) && stage == CJ_STAGE_FINISHED, "the first finish of a claimed job runs");
	Check(!CjFinishAdmit(&stage) && stage == CJ_STAGE_FINISHED, "a second finish releases nothing");
	stage = CJ_STAGE_EMPTY;
	Check(!CjFinishAdmit(&stage) && stage == CJ_STAGE_EMPTY, "a finish of a job never claimed releases nothing");

	const CjReleaseFacts none = { false, false, false };
	CheckFinishTwice("a worker claim finished twice releases once: S,B+,A,L,E,I", CJ_FINISH_WORKER, none, "S,B+,A,L,E,I");
	CheckFinishTwice("a bg content-lost claim finished twice releases once: S,L", CJ_FINISH_BG_CONTENT_LOST, none, "S,L");
	CheckFinishTwice("a bg pipeline-return claim finished twice releases once: S,L", CJ_FINISH_BG_PIPELINE_RETURN, none, "S,L");

	Recorder r = { "", { true, true }, 0 };
	stage = CJ_STAGE_EMPTY;
	CjFinishRun(&stage, CjReleasePlanFor(CJ_FINISH_WORKER, none), Ops(&r));
	Check(r.trace[0] == 0, "a finish of a job never claimed runs no release");
}

int main()
{
	CheckFinishOnce();
	CheckPolicyMatrix();
	const CjReleaseFacts none = { false, false, false };
	const CjReleaseFacts data = { true, false, false };
	const CjReleaseFacts clone = { false, true, false };
	const CjReleaseFacts both = { true, true, false };
	const CjReleaseFacts adj = { false, false, true };
	const CjReleasePoint write[] = { CJ_RELEASE_PIPELINE_WRITE };
	const CjReleasePoint freeClone[] = { CJ_RELEASE_WORKER_CLONE };
	const CjReleasePoint worker[] = { CJ_FINISH_WORKER };
	const CjReleasePoint bgDrop[] = { CJ_FINISH_BG_CONTENT_LOST };
	const CjReleasePoint bgFinished[] = { CJ_FINISH_BG_PIPELINE_RETURN };
	const CjReleasePoint writeWorker[] = { CJ_RELEASE_PIPELINE_WRITE, CJ_FINISH_WORKER };
	const CjReleasePoint cloneWorker[] = { CJ_RELEASE_WORKER_CLONE, CJ_FINISH_WORKER };
	const CjReleasePoint allWorker[] = { CJ_RELEASE_PIPELINE_WRITE, CJ_RELEASE_WORKER_CLONE, CJ_FINISH_WORKER };

	// Eleven exact operation traces for the production-used runner.
	CheckRoute("write absent: no writer", write, 1, none, true, true, "");
	CheckRoute("write present: W", write, 1, data, true, true, "W");
	CheckRoute("clone absent: no Begin", freeClone, 1, none, true, true, "");
	CheckRoute("clone present granted: B+,F,E", freeClone, 1, clone, true, true, "B+,F,E");
	CheckRoute("clone present denied: B-", freeClone, 1, clone, false, true, "B-");
	CheckRoute("worker final granted: S,B+,A,L,E,I", worker, 1, none, true, true, "S,B+,A,L,E,I");
	CheckRoute("worker final denied: S,B-,I", worker, 1, none, false, true, "S,B-,I");
	CheckRoute("BG content lost no adjacency: S,L", bgDrop, 1, none, true, true, "S,L");
	CheckRoute("BG content lost adjacency: D,S,L", bgDrop, 1, adj, true, true, "D,S,L");
	CheckRoute("BG pipeline no adjacency: S,L", bgFinished, 1, none, true, true, "S,L");
	CheckRoute("BG pipeline adjacency: A,S,L", bgFinished, 1, adj, true, true, "A,S,L");

	// Route scripts exercise the runner; they do not prove a game path.
	CheckRoute("preclaim bad-zone original forward: no driver", NULL, 0, none, true, true, "");
	CheckRoute("preclaim type 2/3/4 original forward: no driver", NULL, 0, none, true, true, "");
	CheckRoute("worker direct HIT", worker, 1, none, true, true, "S,B+,A,L,E,I");
	CheckRoute("worker reconstruct failure then late HIT", worker, 1, none, true, true, "S,B+,A,L,E,I");
	CheckRoute("worker reconstruct failure then generation with blob", writeWorker, 2, data, true, true, "W,S,B+,A,L,E,I");
	CheckRoute("worker clone failure then generation", writeWorker, 2, data, true, true, "W,S,B+,A,L,E,I");
	CheckRoute("worker clone failure then stop", worker, 1, none, true, true, "S,B+,A,L,E,I");
	CheckRoute("worker clone-stop free before stop record and final", cloneWorker, 2, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker stale before lookup with clone", cloneWorker, 2, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker reset before lookup with clone", cloneWorker, 2, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker reset before HIT build", worker, 1, none, true, true, "S,B+,A,L,E,I");
	CheckRoute("worker reset before late-HIT build", cloneWorker, 2, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker reset after generation keeps valid blob", allWorker, 3, both, true, true, "W,B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker generation without blob", allWorker, 3, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker lost reentry cleanup", cloneWorker, 2, clone, true, true, "B+,F,E,S,B+,A,L,E,I");
	CheckRoute("worker clone denied then final granted", cloneWorker, 2, clone, false, true, "B-,S,B+,A,L,E,I");
	CheckRoute("worker clone granted then final denied", cloneWorker, 2, clone, true, false, "B+,F,E,S,B-,I");
	CheckRoute("BG content lost no adjacency", bgDrop, 1, none, true, true, "S,L");
	CheckRoute("BG content lost adjacency", bgDrop, 1, adj, true, true, "D,S,L");
	CheckRoute("BG pipeline no adjacency", bgFinished, 1, none, true, true, "S,L");
	CheckRoute("BG pipeline adjacency", bgFinished, 1, adj, true, true, "A,S,L");
	return CheckExit("nm_claimed_job_units");
}
