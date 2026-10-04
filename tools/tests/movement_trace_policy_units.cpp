// The movement trace's rules: the sample distance, the ring keeping the newest, the result ring's
// publish, take, torn slot and overrun, the attribution by the request's start point, the move out of
// the Havok frame, and each trace line round-tripping through the parser.
#include <cstdio>
#include <string.h>
#include "movement/movement_trace_policy.h"

#include "check.h"

static TraceRing       s_ring;
static TraceSample     s_copy[TRACE_RING];
static TraceResultRing s_results;
static TraceResult     s_out;

static void CheckSampling()
{
	CHECK(TraceSampleDue(false, 0.0f, 0.0f, 0.0f, 0.0f, false), "trace: a member with no sample is sampled");
	CHECK(!TraceSampleDue(true, 0.0f, 0.0f, 74.0f, 0.0f, false), "trace: a member 74 units on is not sampled");
	CHECK(TraceSampleDue(true, 0.0f, 0.0f, 75.0f, 0.0f, false), "trace: a member 75 units on is sampled");
	CHECK(TraceSampleDue(true, 0.0f, 0.0f, 0.0f, 0.0f, true), "trace: a state change samples a member that has not moved");
	CHECK((double)TRACE_RING * TRACE_SAMPLE_UNITS >= 250000.0,
	      "trace: the ring holds a whole long order at the sample spacing");
	memset(&s_ring, 0, sizeof(s_ring));
	TraceSample s;
	memset(&s, 0, sizeof(s));
	for (int i = 0; i < TRACE_RING + 5; ++i)
	{
		s.t = (double)i;
		TraceRingPush(&s_ring, s);
	}
	int n = TraceRingCopy(s_ring, s_copy);
	CHECK(n == TRACE_RING && s_copy[0].t == 5.0 && s_copy[TRACE_RING - 1].t == (double)(TRACE_RING + 4)
	      && s_ring.drops == 5, "trace: the ring keeps the newest TRACE_RING");
}

static void Publish(int count)
{
	TraceResult* r = TraceResultBegin(&s_results);
	r->count = count;
	r->copied = count;
	TraceResultEnd(&s_results);
}

static void CheckResultRing()
{
	memset(&s_results, 0, sizeof(s_results));
	long taken = 0, overruns = 0, torn = 0;
	CHECK(!TraceResultTake(&s_results, &taken, &s_out, &overruns, &torn), "trace ring: nothing published reads nothing");
	Publish(7);
	CHECK(TraceResultTake(&s_results, &taken, &s_out, &overruns, &torn) && s_out.count == 7 && taken == 1,
	      "trace ring: a published result is taken once");
	Publish(8);
	s_results.slotSeq[1] += 1;   // a write in progress on the slot about to be read
	CHECK(!TraceResultTake(&s_results, &taken, &s_out, &overruns, &torn) && torn == 1,
	      "trace ring: a slot caught mid-write is skipped");
	s_results.slotSeq[1] += 1;
	memset(&s_results, 0, sizeof(s_results));
	taken = overruns = torn = 0;
	for (int i = 0; i < TRACE_RESULT_RING + 4; ++i)
		Publish(100 + i);
	int got = 0;
	int first = -1;
	while (TraceResultTake(&s_results, &taken, &s_out, &overruns, &torn))
	{
		if (first < 0)
			first = s_out.count;
		++got;
	}
	CHECK(got == TRACE_RESULT_RING && overruns == 4 && first == 104 && torn == 0,
	      "trace ring: a reader 20 behind skips 4 and counts them");
}

static TraceRing   s_attrib[3];
static TraceResult s_result;

static void PushAt(TraceRing* r, double t, float x, float z)
{
	TraceSample s;
	memset(&s, 0, sizeof(s));
	s.t = t;
	s.x = x;
	s.z = z;
	TraceRingPush(r, s);
}

static void CheckAttribution()
{
	memset(s_attrib, 0, sizeof(s_attrib));
	PushAt(&s_attrib[0], 10.0, 0.0f, 0.0f);
	PushAt(&s_attrib[1], 10.0, 100.0f, 0.0f);
	const TraceRing* rings[3] = { &s_attrib[0], &s_attrib[1], NULL };
	const float near40[2] = { 40.0f, 0.0f };
	const float far60[2] = { -60.0f, 0.0f };
	const float onAbsent[2] = { 0.0f, 300.0f };
	CHECK(TraceAttribute(near40, 10.5, rings, 3) == 0, "trace: a request starting 40 units from a member is its");
	CHECK(TraceAttribute(far60, 10.5, rings, 3) == -1,
	      "trace: a request starting 60 units from every member is unmatched");
	CHECK(TraceAttribute(onAbsent, 10.5, rings, 3) == -1, "trace: a member with no sample takes no result");
	const float mid[2] = { 55.0f, 0.0f };
	CHECK(TraceAttribute(mid, 10.5, rings, 3) == 1,
	      "trace: the nearest member within the bound takes the request's result");

	memset(s_attrib, 0, sizeof(s_attrib));
	PushAt(&s_attrib[0], 10.0, 0.0f, 0.0f);
	PushAt(&s_attrib[0], 10.5, 900.0f, 0.0f);
	PushAt(&s_attrib[0], 11.0, 1800.0f, 0.0f);
	const float start[2] = { 10.0f, 0.0f };
	CHECK(TraceAttribute(start, 11.2, rings, 3) == 0,
	      "trace: a result whose member walked on during the latency attributes to its earlier sample");
	CHECK(TraceAttribute(start, 14.0, rings, 3) == -1, "trace: a sample older than the window does not attribute");
	memset(s_attrib, 0, sizeof(s_attrib));
	PushAt(&s_attrib[1], 1.0, 500.0f, 0.0f);
	const float standing[2] = { 510.0f, 0.0f };
	CHECK(TraceAttribute(standing, 20.0, rings, 3) == 1, "trace: a member standing still attributes by its newest sample");

	memset(s_attrib, 0, sizeof(s_attrib));
	PushAt(&s_attrib[0], 10.0, 0.0f, 0.0f);
	PushAt(&s_attrib[0], 10.5, 75.0f, 0.0f);
	const float between[2] = { 37.0f, 0.0f };
	const float aside[2] = { -60.0f, 0.0f };
	CHECK(TRACE_ATTRIB_UNITS > TRACE_SAMPLE_UNITS * 0.5f && TraceAttribute(between, 11.0, rings, 3) == 0,
	      "trace: a request starting 37 units from the nearer of two samples 75 apart attributes");
	CHECK(TraceAttribute(aside, 11.0, rings, 3) == -1, "trace: a request starting 60 units from every sample is unmatched");
	memset(&s_result, 0, sizeof(s_result));
	s_result.copied = 1;
	s_result.shift[0] = 2.0f;
	s_result.shift[2] = -1.0f;
	s_result.start[0] = 2.0f + 3.0f;   // world (30, 0): on the walked line
	s_result.start[2] = -1.0f;
	s_result.mid[0][0] = 2.0f + 3.0f;  // world (30, 60): the first node off to the side
	s_result.mid[0][2] = -1.0f + 6.0f;
	float startXz[2];
	TraceResultStartXz(s_result, startXz);
	CHECK(startXz[0] == 30.0f && startXz[1] == 0.0f && TraceAttribute(startXz, 11.0, rings, 3) == 0,
	      "trace: a result is sought by its request's start, not by its first node");

	const float h[3] = { 12.5f, 1.0f, -3.0f };
	const float shift[3] = { 2.5f, 0.0f, 1.0f };
	float w[3];
	TraceHavokToWorld(h, shift, w);
	CHECK(w[0] == 100.0f && w[1] == 10.0f && w[2] == -40.0f, "trace: a Havok point moves to the world frame");
}

static void CheckLines()
{
	TraceLine line;
	TraceOrderLine o = { 3, -1234.5f, 567.0f, 89.5f, -10.0f, 2, 12.25 };
	std::string ot = TraceFormatOrder(o);
	CHECK(ot == "o order=3 from=(-1234.5,567.0) to=(89.5,-10.0) members=2 t0=12.250", "trace line: the order line's text");
	CHECK(TraceParseLine(ot.c_str(), &line) == TLK_ORDER && line.o.order == 3 && line.o.fromX == -1234.5f
	      && line.o.toZ == -10.0f && line.o.members == 2 && line.o.t0 == 12.25, "trace line: an order round-trips");
	TraceSample s = { 3, 1, 13.5, 100.5f, -2.5f, 300.0f, 3, 1, 31, 30, -1 };
	std::string st = TraceFormatSample(s);
	CHECK(TraceParseLine(st.c_str(), &line) == TLK_SAMPLE && line.s.order == 3 && line.s.member == 1 && line.s.t == 13.5
	      && line.s.x == 100.5f && line.s.y == -2.5f && line.s.z == 300.0f && line.s.pathState == 3
	      && line.s.characterState == 1 && line.s.cellX == 31 && line.s.cellY == 30 && line.s.leg == -1,
	      "trace line: a sample round-trips");
	TraceResultLine p;
	p.order = 3;
	p.member = 1;
	p.t = 14.75;
	p.cut = 1;
	p.count = 300;
	TraceNode a = { 4194305u, 10.5f, 2.0f, -20.5f };
	TraceNode b = { 7u, 11.0f, 2.5f, -21.0f };
	p.nodes.push_back(a);
	p.nodes.push_back(b);
	std::string pt = TraceFormatResult(p);
	CHECK(TraceParseLine(pt.c_str(), &line) == TLK_RESULT && line.p.count == 300 && line.p.cut == 1
	      && line.p.nodes.size() == 2 && line.p.nodes[0].face == 4194305u && line.p.nodes[1].z == -21.0f,
	      "trace line: a result round-trips");
	p.member = -1;
	p.nodes.clear();
	std::string un = TraceFormatResult(p);
	CHECK(un == "p order=3 member=-1 t=14.750 cut=1 n=300" && TraceParseLine(un.c_str(), &line) == TLK_RESULT
	      && line.p.member == -1 && line.p.order == 3 && line.p.nodes.empty(),
	      "trace line: an unmatched result round-trips with member -1");
	TraceLaunchLine l = { 4242u, 2026, 10, 4, 9, 5, 7, 1.5 };
	std::string lt = TraceFormatLaunch(l);
	CHECK(lt == "l pid=4242 armed=2026-10-04 09:05:07 t=1.500", "trace line: the launch line's text");
	CHECK(TraceParseLine(lt.c_str(), &line) == TLK_LAUNCH && line.l.pid == 4242u && line.l.year == 2026
	      && line.l.month == 10 && line.l.day == 4 && line.l.hour == 9 && line.l.minute == 5 && line.l.second == 7
	      && line.l.t == 1.5, "trace line: a launch line round-trips");
	CHECK(TraceParseLine("l pid=4242 armed=2026-10-04", &line) == TLK_NONE, "trace line: a cut launch line reads none");
	CHECK(TraceParseLine("", &line) == TLK_NONE && TraceParseLine("s order=x", &line) == TLK_NONE
	      && TraceParseLine((pt + " junk").c_str(), &line) == TLK_NONE, "trace line: a blank or malformed line reads none");
}

int main()
{
	CheckSampling();
	CheckResultRing();
	CheckAttribution();
	CheckLines();
	return CheckExit("movement_trace_policy_units");
}
