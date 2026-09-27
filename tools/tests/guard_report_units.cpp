// The guards' shared reporting against literal heartbeat lines: each golden line is the text the
// guard's own hand-built heartbeat printed for the same counter values, written out here as a
// literal and never rebuilt by the code under test. Also the heartbeat timer, the call sample and
// the fire-line cap.

#include "check.h"
#include "fixes/guard_report.h"
#include <string.h>

static void TestGoldenGraphExpand()
{
	static volatile LONG v[16] = { 7, 6, 1, 0, 0, 0, 2, 1, 1, 0, 0, 1, 1, -1, 12, 345 };
	const GuardCounter rows[16] =
	{
		{ "calls",       GF_COUNT, &v[0],  0 },
		{ "judged",      GF_COUNT, &v[1],  0 },
		{ "unjudged",    GF_COUNT, &v[2],  0 },
		{ "noOpenSet",   GF_COUNT, &v[3],  0 },
		{ "noVisitor",   GF_COUNT, &v[4],  0 },
		{ "noArray",     GF_COUNT, &v[5],  0 },
		{ "fired",       GF_COUNT, &v[6],  0 },
		{ "badSection",  GF_COUNT, &v[7],  0 },
		{ "noInstance",  GF_COUNT, &v[8],  0 },
		{ "noNodes",     GF_COUNT, &v[9],  0 },
		{ "noPositions", GF_COUNT, &v[10], 0 },
		{ "fireCache",   GF_COUNT, &v[11], 0 },
		{ "fireLookup",  GF_COUNT, &v[12], 0 },
		{ "firstSec",    GF_COUNT, &v[13], 0 },
		{ "lastSec",     GF_COUNT, &v[14], 0 },
		{ "lastIdx",     GF_COUNT, &v[15], 0 },
	};
	FixedLogBufN<384> o;
	GuardHeartbeatBegin(&o, "GraphExpandGuard running:");
	GuardFields(&o, rows, 16, true);
	const char* got = FlbDone(&o);
	CHECK(strcmp(got,
		"GraphExpandGuard running: calls=7 judged=6 unjudged=1 noOpenSet=0 noVisitor=0 noArray=0 fired=2 badSection=1 noInstance=1 noNodes=0 noPositions=0 fireCache=1 fireLookup=1 firstSec=-1 lastSec=12 lastIdx=345") == 0,
		"golden 1: GraphExpandGuard running line, 16 counters, live");
}

static void TestGoldenCreateInstanceNotLive()
{
	static volatile LONG calls = 5, fromGen = 4, lines = 3;
	const GuardCounter rows[3] =
	{
		{ "calls",   GF_COUNT,    &calls,   0 },
		{ "fromGen", GF_COUNT,    &fromGen, 0 },
		{ "lines",   GF_COUNT_OF, &lines,   32 },
	};
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "CreateInstanceGuard running:");
	FlbStr(&o, " installed=no("); FlbStr(&o, "hook"); FlbStr(&o, ")");
	FlbStr(&o, " mode="); FlbStr(&o, "observe");
	GuardFields(&o, rows, 3, false);
	const char* got = FlbDone(&o);
	CHECK(strcmp(got,
		"CreateInstanceGuard running: installed=no(hook) mode=observe calls=? fromGen=? lines=?/32") == 0,
		"golden 2: CreateInstanceGuard running line, not live, hand-built prefix and ?/32");
}

static void TestGoldenMeshFace()
{
	static volatile LONG calls = 3, judged = 3, lines = 1, firstSec = -1;
	const GuardCounter rows[4] =
	{
		{ "calls",    GF_COUNT,    &calls,    0 },
		{ "judged",   GF_COUNT,    &judged,   0 },
		{ "lines",    GF_COUNT_OF, &lines,    32 },
		{ "firstSec", GF_COUNT,    &firstSec, 0 },
	};
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "MeshFaceGuard running:");
	GuardFields(&o, rows, 4, true);
	const char* got = FlbDone(&o);
	CHECK(strcmp(got, "MeshFaceGuard running: calls=3 judged=3 lines=1/32 firstSec=-1") == 0,
		"golden 3: MeshFaceGuard running line, live, lines=1/32 then a negative counter");
}

static void TestTimer()
{
	LARGE_INTEGER before;
	QueryPerformanceCounter(&before);

	volatile LONGLONG nextBeat = 0;
	CHECK(GuardBeatDue(&nextBeat, 10000000, 60), "timer: the first call is due");
	LARGE_INTEGER after;
	QueryPerformanceCounter(&after);
	// The beat moves qpf * seconds past the clock read inside the call, which
	// lies between the two reads here.
	CHECK(nextBeat >= before.QuadPart + 10000000LL * 60 && nextBeat <= after.QuadPart + 10000000LL * 60,
		"timer: a due call moves the next beat qpf*seconds past now");
	CHECK(!GuardBeatDue(&nextBeat, 10000000, 60), "timer: a second call at once is not due");

	volatile LONGLONG never = 0;
	CHECK(GuardBeatDue(&never, 0, 60), "timer: with no frequency the first call is due");
	CHECK(never == _I64_MAX, "timer: with no frequency the next beat is never");
	CHECK(!GuardBeatDue(&never, 0, 60), "timer: with no frequency a second call is not due");

	CHECK(GuardBeatSample(1), "sample: call 1");
	CHECK(GuardBeatSample(1024), "sample: call 1024");
	CHECK(GuardBeatSample(2048), "sample: call 2048");
	CHECK(!GuardBeatSample(2), "sample: not call 2");
	CHECK(!GuardBeatSample(1023), "sample: not call 1023");
	CHECK(!GuardBeatSample(1025), "sample: not call 1025");
}

static void TestClaim()
{
	volatile LONG lines = 0;
	CHECK(GuardFireClaim(&lines, 2), "claim: first under the cap");
	CHECK(GuardFireClaim(&lines, 2), "claim: second under the cap");
	CHECK(!GuardFireClaim(&lines, 2), "claim: third refused at the cap");
	CHECK(!GuardFireClaim(&lines, 2), "claim: fourth refused at the cap");
	CHECK(lines == 2, "claim: the counter stops at the cap");
}

int main()
{
	TestGoldenGraphExpand();
	TestGoldenCreateInstanceNotLive();
	TestGoldenMeshFace();
	TestTimer();
	TestClaim();
	return CheckExit("guard_report_units");
}
