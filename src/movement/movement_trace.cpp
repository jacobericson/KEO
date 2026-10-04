// movement_trace.cpp - The movement trace: per-member samples by distance moved, the path results the
// path thread copied attributed to the nearest member, and each closed order's lines appended to the
// trace file beside the log in one write. Main thread; the path-thread half is path_result_trace.cpp.
// The release build compiles the whole body out.
#include "movement/movement_trace.h"
#ifdef KEO_DEBUG
#include "movement/movement_config.h"
#include "movement/order_outcome_table.h"
#include "movement/char_movement_fields.h"
#include "pathfind/path_result_trace.h"
#include "planner/plan_store.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include <new>
#include <stdio.h>
#include <string.h>

namespace movement_trace_detail {

struct TraceMember
{
	uintptr_t  character;   // 0: a free entry
	int        order;       // the order-outcome record it is sampled under
	int        index;       // its member number within that order
	TraceRing* ring;        // made at its first sample
	float      lastX, lastZ;
	int        haveLast;
};

struct TraceOrder
{
	int                           order;     // 0: a free entry
	TraceOrderLine                head;
	std::vector<TraceResultLine>* results;   // made at its first result
};

} // namespace movement_trace_detail
using namespace movement_trace_detail;

static TraceMember  s_members[TRACE_MEMBERS];
static TraceOrder   s_orders[TRACE_MEMBERS];
static TraceSample  s_copy[TRACE_RING];
static TraceResult  s_take;
static char         s_path[MAX_PATH];
static int          s_armState = 0;   // 0 not tried, 1 armed, -1 off
static int          s_armed = 0;
static int          s_resultsHook = 0;
static long         s_taken = 0, s_overruns = 0, s_torn = 0;
static long         s_samples = 0, s_written = 0, s_bytes = 0, s_drops = 0, s_results = 0, s_cut = 0;
static long         s_unmatched = 0, s_openFail = 0;
static double       s_lastLine = 0.0;

// The trace arms once, at its first use on the main thread: the key at on (the release build has no
// body to arm).
static void ArmOnce()
{
	if (s_armState != 0)
		return;
	s_armState = -1;
	if (movement::g_movementCfg.cfg_movementTrace != TRACE_ON)
		return;
	_snprintf_s(s_path, sizeof(s_path), _TRUNCATE, "%sKEO.trace.txt", GetDLLDirectory().c_str());
	s_resultsHook = HookRowInstalled(HOOK_CS_FIND_PATH) && HookRowInstalled(HOOK_CS_FIND_PATH_FALLBACK);
	PathResultTraceArm();
	s_armState = 1;
	s_armed = 1;
	LogMsg(s_resultsHook ? "Trace: armed file=KEO.trace.txt resultsHook=ok"
	                     : "Trace: armed file=KEO.trace.txt resultsHook=off");
}

static TraceOrder* FindOrder(int order)
{
	for (int i = 0; i < TRACE_MEMBERS; ++i)
		if (s_orders[i].order == order)
			return &s_orders[i];
	return NULL;
}

// The order's entry, made with its coordinates at its first member; NULL when the table is full.
static TraceOrder* OrderFor(int order, double now)
{
	TraceOrder* o = FindOrder(order);
	if (o)
		return o;
	o = FindOrder(0);
	if (!o)
		return NULL;
	memset(o, 0, sizeof(*o));
	o->order = order;
	o->head.order = order;
	o->head.t0 = now;
	float xy[4];
	if (OOT_OrderCoords(order, xy))
	{
		o->head.fromX = xy[0];
		o->head.fromZ = xy[1];
		o->head.toX = xy[2];
		o->head.toZ = xy[3];
	}
	return o;
}

// The member's entry under its current order, made with the next member number; NULL when full.
static TraceMember* MemberFor(uintptr_t character, int order, double now)
{
	TraceMember* freeEntry = NULL;
	for (int i = 0; i < TRACE_MEMBERS; ++i)
	{
		TraceMember& m = s_members[i];
		if (m.character == character && m.order == order)
			return &m;
		if (!m.character && !freeEntry)
			freeEntry = &m;
	}
	TraceOrder* o = freeEntry ? OrderFor(order, now) : NULL;
	if (!o)
		return NULL;
	memset(freeEntry, 0, sizeof(*freeEntry));
	freeEntry->character = character;
	freeEntry->order = order;
	freeEntry->index = o->head.members++;
	return freeEntry;
}

static int LegOf(uintptr_t cm)
{
	int slot = planner::PlanStoreFind(cm);
	planner::PlanView v;
	if (slot >= 0 && planner::PlanStoreRead(slot, &v) && v.cm == cm)
		return v.legIndex;
	return -1;
}

void MovementTraceSample(uintptr_t character, uintptr_t cm, uintptr_t hc, int characterState)
{
	ArmOnce();
	if (!s_armed || !character || !cm || !hc)
		return;
	int order = OOT_OrderOf((size_t)character);
	if (order <= 0)
		return;
	float x = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_x, OFF_CHAR_POS_X));
	float y = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_y, OFF_CHAR_POS_Y));
	float z = *(float*)(KLIB_MEMBER(3, character, RootObjectBase_pos_z, OFF_CHAR_POS_Z));
	double now = ElapsedSec();
	TraceMember* m = MemberFor(character, order, now);
	if (!m)
	{
		++s_drops;
		return;
	}
	if (!TraceSampleDue(m->haveLast != 0, m->lastX, m->lastZ, x, z))
		return;
	if (!m->ring)
	{
		m->ring = new (std::nothrow) TraceRing;
		if (!m->ring)
		{
			++s_drops;
			return;
		}
		memset(m->ring, 0, sizeof(*m->ring));
	}
	TraceSample s;
	s.order = order;
	s.member = m->index;
	s.t = now;
	s.x = x;
	s.y = y;
	s.z = z;
	s.pathState = *(int*)(KLIB_MEMBER(3, hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));
	s.characterState = characterState;
	planner::PlanCellOf(x, z, &s.cellX, &s.cellY);
	s.leg = LegOf(cm);
	long before = m->ring->drops;
	TraceRingPush(m->ring, s);
	s_drops += m->ring->drops - before;
	m->lastX = x;
	m->lastZ = z;
	m->haveLast = 1;
	++s_samples;
}

// One copied result, in the world frame, to the member nearest its first point.
static void Attribute(const TraceResult& r)
{
	++s_results;
	if (r.cut)
		++s_cut;
	if (r.copied <= 0)
		return;
	float first[3];
	TraceHavokToWorld(r.mid[0], r.shift, first);
	float last[2 * TRACE_MEMBERS];
	int have[TRACE_MEMBERS];
	for (int i = 0; i < TRACE_MEMBERS; ++i)
	{
		last[2 * i] = s_members[i].lastX;
		last[2 * i + 1] = s_members[i].lastZ;
		have[i] = s_members[i].character && s_members[i].haveLast;
	}
	const float firstXz[2] = { first[0], first[2] };
	int who = TraceAttribute(firstXz, last, have, TRACE_MEMBERS);
	TraceOrder* o = who >= 0 ? FindOrder(s_members[who].order) : NULL;
	if (!o)
	{
		++s_unmatched;
		return;
	}
	if (!o->results)
		o->results = new (std::nothrow) std::vector<TraceResultLine>;
	if (!o->results || (int)o->results->size() >= TRACE_ORDER_RESULTS)
	{
		++s_drops;
		return;
	}
	o->results->push_back(TraceResultLine());
	TraceResultLine& p = o->results->back();
	p.order = o->order;
	p.member = s_members[who].index;
	p.t = r.t;
	p.cut = r.cut;
	p.count = r.count;
	p.nodes.resize(r.copied);
	for (int i = 0; i < r.copied; ++i)
	{
		float w[3];
		TraceHavokToWorld(r.mid[i], r.shift, w);
		p.nodes[i].face = r.face[i];
		p.nodes[i].x = w[0];
		p.nodes[i].y = w[1];
		p.nodes[i].z = w[2];
	}
}

void MovementTraceFrame(double now)
{
	ArmOnce();
	if (!s_armed)
		return;
	while (PathResultTraceTake(&s_taken, &s_take, &s_overruns, &s_torn))
		Attribute(s_take);
	if (now - s_lastLine < TRACE_LINE_SECONDS)
		return;
	s_lastLine = now;
	char line[384];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "Trace: samples=%ld orders=%ld bytes=%ld traceDrop=%ld results=%ld cut=%ld traceUnmatched=%ld"
	            " overruns=%ld torn=%ld traceOpenFail=%ld resultsHook=%s",
	            s_samples, s_written, s_bytes, s_drops, s_results, s_cut, s_unmatched, s_overruns, s_torn,
	            s_openFail, s_resultsHook ? "ok" : "off");
	LogMsg(line);
}

// Frees one order's entries: its members' rings and its results.
static void FreeOrder(TraceOrder* o)
{
	for (int i = 0; i < TRACE_MEMBERS; ++i)
	{
		if (!s_members[i].character || s_members[i].order != o->order)
			continue;
		delete s_members[i].ring;
		memset(&s_members[i], 0, sizeof(s_members[i]));
	}
	delete o->results;
	memset(o, 0, sizeof(*o));
}

void MovementTraceOnOrderClose(int orderNum)
{
	if (!s_armed)
		return;
	TraceOrder* o = FindOrder(orderNum);
	if (!o)
		return;
	std::string text = TraceFormatOrder(o->head) + "\n";
	for (int i = 0; i < TRACE_MEMBERS; ++i)
	{
		const TraceMember& m = s_members[i];
		if (!m.character || m.order != orderNum || !m.ring)
			continue;
		int n = TraceRingCopy(*m.ring, s_copy);
		for (int k = 0; k < n; ++k)
			text += TraceFormatSample(s_copy[k]) + "\n";
	}
	for (size_t r = 0; o->results && r < o->results->size(); ++r)
		text += TraceFormatResult((*o->results)[r]) + "\n";
	FILE* f = NULL;
	if (fopen_s(&f, s_path, "ab") != 0 || !f)
		++s_openFail;
	else
	{
		fwrite(text.c_str(), 1, text.size(), f);
		fclose(f);
		s_bytes += (long)text.size();
		++s_written;
	}
	FreeOrder(o);
}

void MovementTraceReset()
{
	if (!s_armed)
		return;
	for (int i = 0; i < TRACE_MEMBERS; ++i)
		if (s_orders[i].order)
			FreeOrder(&s_orders[i]);
	for (int i = 0; i < TRACE_MEMBERS; ++i)
	{
		delete s_members[i].ring;
		memset(&s_members[i], 0, sizeof(s_members[i]));
	}
}

#endif // KEO_DEBUG
