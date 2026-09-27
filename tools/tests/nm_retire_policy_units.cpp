// The navmesh worker retire through the shipped policy: the decision, the
// slice arithmetic, the three record lines and the loop over a scripted fake
// that stands in for the clock, the workers, the log and the process.

#include <cstdio>
#include <cstring>
#include "navmesh/workers/nm_retire_policy.h"

#include "check.h"

struct Fake
{
	unsigned   clock;
	int        live;
	int        waits;
	unsigned   waitArg[16];
	RetireWait result;
	int        joinOnWait;       // the wait (1-based) that joins every worker; 0: none
	unsigned   joinAdvance;      // the clock's advance on that wait
	int        dropLiveOnWait;   // the wait (1-based) after which no worker is live; 0: none
	unsigned   shortBy;          // a timed-out wait returns this many ms early
	bool       logDown;
	int        logCalls;
	int        nLines;
	char       lines[16][384];
	int        nFallback;
	char       fallback[16][384];
	int        terminateCalls;
	unsigned   terminateCode;
};

// A runaway loop ends here rather than hanging the suite: its checks fail on
// the wait count.
static const int FAKE_WAIT_LIMIT = 64;

static void FakeInit(Fake* f)
{
	memset(f, 0, sizeof(*f));
	f->result = RETIRE_WAIT_TIMEOUT;
}

static void FakeStore(char (*rows)[384], int* n, const char* line)
{
	if (*n < 16)
		_snprintf_s(rows[*n], 384, _TRUNCATE, "%s", line);
	++*n;
}

static unsigned FNow(void* ctx)
{
	return ((Fake*)ctx)->clock;
}

static int FLive(void* ctx)
{
	return ((Fake*)ctx)->live;
}

static RetireWait FWait(void* ctx, unsigned ms, unsigned long* gle)
{
	Fake* f = (Fake*)ctx;
	++f->waits;
	if (f->waits <= 16)
		f->waitArg[f->waits - 1] = ms;
	if (f->waits >= FAKE_WAIT_LIMIT)
		f->live = 0;
	if (f->waits == f->dropLiveOnWait)
		f->live = 0;
	if (f->waits == f->joinOnWait)
	{
		f->clock += f->joinAdvance;
		f->live = 0;
		return RETIRE_WAIT_JOINED;
	}
	if (f->result == RETIRE_WAIT_TIMEOUT)
		f->clock += ms - f->shortBy;
	else if (f->result == RETIRE_WAIT_FAILED)
		*gle = 6;
	return f->result;
}

static long FStopDrops(void*)
{
	return 0;
}

static void FPhases(void*, char* out, size_t cap)
{
	_snprintf_s(out, cap, _TRUNCATE, "%s", "w3:generating");
}

static bool FLog(void* ctx, const char* line)
{
	Fake* f = (Fake*)ctx;
	++f->logCalls;
	if (f->logDown)
		return false;
	FakeStore(f->lines, &f->nLines, line);
	return true;
}

static void FFallback(void* ctx, const char* line)
{
	Fake* f = (Fake*)ctx;
	FakeStore(f->fallback, &f->nFallback, line);
}

static void FTerminate(void* ctx, unsigned code)
{
	Fake* f = (Fake*)ctx;
	++f->terminateCalls;
	f->terminateCode = code;
}

static RetireOps FakeOps(Fake* f)
{
	RetireOps ops;
	ops.ctx         = f;
	ops.nowMs       = FNow;
	ops.liveCount   = FLive;
	ops.waitSlice   = FWait;
	ops.stopDrops   = FStopDrops;
	ops.phases      = FPhases;
	ops.log         = FLog;
	ops.logFallback = FFallback;
	ops.terminate   = FTerminate;
	return ops;
}

static RetireLineInput LineInput(RetireAction action, int slice, unsigned elapsedMs, int live,
                                 long stopDrop, bool waitFailed, unsigned long gle, const char* phases)
{
	RetireLineInput in;
	in.action     = action;
	in.slice      = slice;
	in.elapsedMs  = elapsedMs;
	in.live       = live;
	in.stopDrop   = stopDrop;
	in.waitFailed = waitFailed;
	in.gle        = gle;
	in.phases     = phases;
	return in;
}

static RetireSummary Summary(int activeCount, unsigned waitMs, bool anyWaitFailed, unsigned long lastGle,
                             long stopDrop, long cleanupLeft)
{
	RetireSummary s;
	s.activeCount   = activeCount;
	s.waitMs        = waitMs;
	s.anyWaitFailed = anyWaitFailed;
	s.lastGle       = lastGle;
	s.live          = 0;
	s.stopDrop      = stopDrop;
	s.cleanupLeft   = cleanupLeft;
	return s;
}

struct DecideRow
{
	unsigned     elapsedMs;
	int          live;
	bool         waitFailed;
	RetireAction want;
	const char*  what;
};

static const DecideRow kDecide[] =
{
	{     0,  0, false, RETIRE_PROCEED,      "decide: no worker live at the stop proceeds at once" },
	{     0,  0, true,  RETIRE_PROCEED,      "decide: a failed wait whose live count reads 0 proceeds" },
	{ 44999,  0, false, RETIRE_PROCEED,      "decide: the last worker gone just before the cap proceeds" },
	{ 45000,  0, false, RETIRE_PROCEED,      "decide: a live count of 0 at the cap proceeds, never terminates" },
	{     0, -1, false, RETIRE_PROCEED,      "decide: a negative live count reads as none" },
	{     0,  1, false, RETIRE_WAIT_SLICE,   "decide: a live worker at the stop waits a slice" },
	{  4999,  1, false, RETIRE_WAIT_SLICE,   "decide: just before the first boundary waits" },
	{  5000,  1, false, RETIRE_WAIT_SLICE,   "decide: the first boundary waits another slice" },
	{ 14999,  6, false, RETIRE_WAIT_SLICE,   "decide: just under the report threshold waits unflagged" },
	{ 15000,  1, false, RETIRE_REPORT_SLICE, "decide: the report threshold flags the wait as a hang" },
	{ 44999,  1, false, RETIRE_REPORT_SLICE, "decide: just under the cap still waits, flagged" },
	{ 45000,  1, false, RETIRE_TERMINATE,    "decide: the cap terminates" },
	{ 90000,  2, false, RETIRE_TERMINATE,    "decide: past the cap terminates" },
	{     0,  1, true,  RETIRE_REPORT_SLICE, "decide: a failed wait with a live worker never proceeds" },
	{ 10000,  1, true,  RETIRE_REPORT_SLICE, "decide: a failed wait below the threshold is a report" },
	{ 45000,  1, true,  RETIRE_TERMINATE,    "decide: a failed wait at the cap terminates" },
};

static const char* const kSlice1 =
	"NavMesh workers still live at NavMesh::stop: retireSlice=1 joined=TIMEOUT waitMs=5000/45000 live=1 stopDrop=0 phases=[w3:generating]";
static const char* const kSlice2 =
	"NavMesh workers still live at NavMesh::stop: retireSlice=2 joined=TIMEOUT waitMs=10000/45000 live=1 stopDrop=0 phases=[w3:generating]";
static const char* const kSlice3Hang =
	"NavMesh workers still live at NavMesh::stop: retireSlice=3 joined=HANG waitMs=15000/45000 live=1 stopDrop=0 phases=[w3:generating]";
static const char* const kSliceFailed =
	"NavMesh workers still live at NavMesh::stop: retireSlice=1 joined=FAILED waitMs=5000/45000 gle=6 live=2 stopDrop=4 phases=[w0:storing w3:generating]";
static const char* const kRunFailed =
	"NavMesh workers still live at NavMesh::stop: retireSlice=1 joined=FAILED waitMs=5000/45000 gle=6 live=1 stopDrop=0 phases=[w3:generating]";
static const char* const kTerminate =
	"RETIRE TERMINATE: exit code 3, retireSlice=9 joined=HANG waitMs=45000/45000 live=1 stopDrop=0 phases=[w3:generating]";

static bool LineIs(const char* line, const char* want)
{
	return strcmp(line, want) == 0;
}

int main()
{
	// A. The decision.
	for (size_t i = 0; i < sizeof(kDecide) / sizeof(kDecide[0]); ++i)
	{
		const DecideRow& row = kDecide[i];
		Check(RetireDecide(row.elapsedMs, row.live, row.waitFailed) == row.want, row.what);
	}

	// B. The next wait.
	Check(RetireNextWaitMs(0) == 5000, "next wait: a whole slice at the stop");
	Check(RetireNextWaitMs(12000) == 5000, "next wait: a whole slice mid-budget");
	Check(RetireNextWaitMs(42000) == 3000, "next wait: the last slice stops at the cap");
	Check(RetireNextWaitMs(45000) == 0, "next wait: none at the cap");
	Check(RetireNextWaitMs(50000) == 0, "next wait: none past the cap");

	// C. The record lines.
	{
		char buf[RETIRE_LINE_CHARS];
		const RetireLineInput first = LineInput(RETIRE_WAIT_SLICE, 1, 5000, 1, 0, false, 0, "w3:generating");

		RetireFormatSliceLine(buf, sizeof(buf), first);
		Check(LineIs(buf, kSlice1), "slice line: an unflagged slice");

		RetireFormatSliceLine(buf, sizeof(buf),
			LineInput(RETIRE_REPORT_SLICE, 3, 15000, 1, 0, false, 0, "w3:generating"));
		Check(LineIs(buf, kSlice3Hang), "slice line: the report threshold reads joined=HANG");

		RetireFormatSliceLine(buf, sizeof(buf),
			LineInput(RETIRE_REPORT_SLICE, 1, 5000, 2, 4, true, 6, "w0:storing w3:generating"));
		Check(LineIs(buf, kSliceFailed), "slice line: a failed wait carries joined=FAILED and gle");

		RetireFormatTerminateLine(buf, sizeof(buf),
			LineInput(RETIRE_TERMINATE, 9, 45000, 1, 0, false, 0, "w3:generating"));
		Check(LineIs(buf, kTerminate), "terminate line: the final record");

		RetireFormatRetiredLine(buf, sizeof(buf), Summary(3, 1, false, 0, 0, 0));
		Check(LineIs(buf, "NavMesh workers retired at NavMesh::stop: 3 joined=ok waitMs=1/45000 live=0 stopDrop=0"),
		      "retired line: a fast join");

		RetireFormatRetiredLine(buf, sizeof(buf), Summary(3, 16604, false, 0, 2, 0));
		Check(LineIs(buf, "NavMesh workers retired at NavMesh::stop: 3 joined=HANG waitMs=16604/45000 live=0 stopDrop=2"),
		      "retired line: a join past the report threshold reads joined=HANG");

		RetireFormatRetiredLine(buf, sizeof(buf), Summary(2, 5000, true, 6, 0, 0));
		Check(LineIs(buf, "NavMesh workers retired at NavMesh::stop: 2 joined=FAILED waitMs=5000/45000 gle=6 live=0 stopDrop=0"),
		      "retired line: a failed wait reads joined=FAILED with gle");

		RetireFormatRetiredLine(buf, sizeof(buf), Summary(0, 0, false, 0, 0, 0));
		Check(LineIs(buf, "NavMesh workers retired at NavMesh::stop: 0 joined=ok waitMs=0/45000 live=0 stopDrop=0"),
		      "retired line: no pool");

		RetireFormatRetiredLine(buf, sizeof(buf), Summary(3, 5000, false, 0, 0, 2));
		Check(LineIs(buf, "NavMesh workers retired at NavMesh::stop: 3 joined=ok waitMs=5000/45000 live=0 stopDrop=0 cleanupLeft=2"),
		      "retired line: an outstanding release prints cleanupLeft");

		char small[16];
		const size_t n = RetireFormatSliceLine(small, sizeof(small), first);
		Check(n == 15 && strlen(small) == 15 && LineIs(small, "NavMesh workers"),
		      "format: a short buffer truncates and terminates");
	}

	// D. The loop over the fake.
	{
		Fake f;
		FakeInit(&f);
		RetireRun(FakeOps(&f));
		Check(f.waits == 0 && f.nLines == 0 && f.nFallback == 0 && f.terminateCalls == 0,
		      "run: no worker live: no wait, no line, no terminate");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		const RetireResult r = RetireRun(FakeOps(&f));
		bool allSlices = f.waits == 9;
		for (int i = 0; i < 9 && i < 16; ++i)
			allSlices = allSlices && f.waitArg[i] == 5000;
		Check(allSlices, "run: a stall waits nine slices of 5000 ms");
		Check(f.nLines == 9 && strncmp(f.lines[8], "RETIRE TERMINATE:", 17) == 0,
		      "run: a stall writes eight slice lines, then the final record");
		Check(LineIs(f.lines[1], kSlice2), "run: the second slice line is unflagged");
		Check(LineIs(f.lines[2], kSlice3Hang), "run: the third slice line flags the hang");
		Check(LineIs(f.lines[8], kTerminate), "run: the final record ends the stall");
		Check(f.terminateCalls == 1 && f.terminateCode == 3 && r.terminated,
		      "run: a stall terminates once, with exit code 3");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.joinOnWait = 4;
		f.joinAdvance = 1604;
		const RetireResult r = RetireRun(FakeOps(&f));
		Check(f.waits == 4 && f.terminateCalls == 0,
		      "run: a join after the threshold proceeds after four waits, without terminating");
		Check(f.nLines == 3, "run: a join after the threshold leaves three slice lines");
		Check(r.waitMs == 16604, "run: the retire's wait time is the clock's at the join");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.result = RETIRE_WAIT_FAILED;
		RetireRun(FakeOps(&f));
		Check(f.waits == 9 && f.terminateCalls == 1,
		      "run: failed waits are charged a slice each and reach the cap");
		Check(LineIs(f.lines[0], kRunFailed), "run: a failed wait's line carries joined=FAILED and gle");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.result = RETIRE_WAIT_FAILED;
		f.dropLiveOnWait = 1;
		const RetireResult r = RetireRun(FakeOps(&f));
		Check(f.waits == 1 && f.nLines == 0 && f.nFallback == 0 && f.terminateCalls == 0
		      && r.anyWaitFailed && r.lastGle == 6,
		      "run: a failed wait, then no live worker, proceeds");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.logDown = true;
		RetireRun(FakeOps(&f));
		Check(f.logCalls == 1 && f.nFallback == 9 && f.nLines == 0,
		      "run: after one refused log take every later line goes to the fallback");
		Check(f.terminateCalls == 1, "run: a refused log take does not stop the terminate");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.result = RETIRE_WAIT_JOINED;
		RetireRun(FakeOps(&f));
		Check(f.waits == 9 && f.terminateCalls == 1,
		      "run: a join the live count does not confirm is charged and cannot spin");
	}
	{
		Fake f;
		FakeInit(&f);
		f.live = 1;
		f.shortBy = 5;
		RetireRun(FakeOps(&f));
		Check(f.waits == 9 && f.terminateCalls == 1 && strstr(f.lines[8], "waitMs=45000/45000") != NULL,
		      "run: a timer that fires early still ends at the ninth slice");
	}

	// E. The emit.
	{
		Fake f;
		FakeInit(&f);
		const RetireOps ops = FakeOps(&f);
		bool stuck = false;
		RetireEmit(ops, &stuck, "line one");
		Check(f.logCalls == 1 && f.nLines == 1 && !stuck, "emit: a line goes to the log while it answers");
	}
	{
		Fake f;
		FakeInit(&f);
		f.logDown = true;
		const RetireOps ops = FakeOps(&f);
		bool stuck = false;
		RetireEmit(ops, &stuck, "line one");
		Check(stuck && f.logCalls == 1 && f.nFallback == 1,
		      "emit: a refused take marks the log stuck and writes the fallback");
		RetireEmit(ops, &stuck, "line two");
		Check(f.logCalls == 1 && f.nFallback == 2, "emit: once stuck, the log is not tried again");
	}

	return CheckExit("nm_retire_policy_units");
}
