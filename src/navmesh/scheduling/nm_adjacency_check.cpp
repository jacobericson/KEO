// nm_adjacency_check.cpp - lock-free adjacency registry snapshot checker.
// Stitching threads read the registry without +152, update Interlocked counters,
// and write only bounded deferred violation diagnostics.
#include "navmesh/scheduling/nm_adjacency_internal.h"
#include "base/fixed_log_buf.h"
#include <string.h>
using namespace nm_adjacency_detail;
// ---------------------------------------------------------------------------
// Checker: a stitch whose job conflicts with another live job is a hole.
// ---------------------------------------------------------------------------

static bool Snapshot(NmAdjEntry* out, int* high)
{
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		LONG s1 = Read(&g_regSeq);
		if (s1 & 1) { YieldProcessor(); continue; }
		int h = g_reg.high;
		if (h < 0) h = 0;
		if (h > NMADJ_CAP) h = NMADJ_CAP;
		memcpy(out, (const void*)g_reg.e, sizeof(NmAdjEntry) * h);
		MemoryBarrier();
		if (Read(&g_regSeq) == s1)
		{
			*high = h;
			return true;
		}
	}
	return false;
}

static bool ReadUid(const void* nav, int* uid)
{
	unsigned int v = 0;
	if (!nav || !ReadU32Guarded((const unsigned char*)nav + OFF_NAVINST_UID, &v))
		return false;
	*uid = (int)v;
	return true;
}

static const char* StateName(int s)
{
	switch (s)
	{
	case NMADJ_CLAIMED:   return "claimed";
	case NMADJ_PUBLISHED: return "published";
	case NMADJ_RESERVED:  return "reserved";
	default:              return "free";
	}
}

static void ViolLine(const char* what, const NmAdjEntry& own, const NmAdjEntry& other, int cellX, int cellY)
{
	for (;;)
	{
		LONG cur = Read(&s_violLines);
		if (cur >= kMaxViolLines)
			return;
		if (InterlockedCompareExchange(&s_violLines, cur + 1, cur) == cur)
			break;
	}
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "NavMeshAdj VIOLATION: "); FlbStr(&o, what);
	FlbStr(&o, " own=");   FlbDec(&o, own.d.x);   FlbChar(&o, ','); FlbDec(&o, own.d.y);
	FlbStr(&o, " t");      FlbDec(&o, own.d.type);
	FlbStr(&o, " other="); FlbDec(&o, other.d.x); FlbChar(&o, ','); FlbDec(&o, other.d.y);
	FlbStr(&o, " t");      FlbDec(&o, other.d.type);
	FlbStr(&o, " ");       FlbStr(&o, StateName(other.state));
	if (cellX != -1000)
	{
		FlbStr(&o, " cell="); FlbDec(&o, cellX); FlbChar(&o, ','); FlbDec(&o, cellY);
	}
	FlbStr(&o, " mode="); FlbStr(&o, s_mode == MODE_ENFORCE ? "enforce" : "count");
	FlbStr(&o, " tid=");  FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

void NmAdjCheckStitch(const void* a, const void* b, bool splice)
{
	if (s_mode == MODE_OFF)
		return;
	InterlockedIncrement(&s_checks);
	const int own = t_own;
	if (own < 0)
	{
		InterlockedIncrement(&s_unowned);
		return;
	}
	NmAdjEntry snap[NMADJ_CAP];
	int high = 0;
	if (!Snapshot(snap, &high))
	{
		InterlockedIncrement(&s_torn);
		return;
	}
	if (own >= high || snap[own].task != t_ownTask || snap[own].state != NMADJ_CLAIMED)
	{
		InterlockedIncrement(&s_unowned);
		return;
	}
	const NmAdjEntry& me = snap[own];

	bool viol = false;
	for (int i = 0; i < high && !viol; ++i)
	{
		if (i == own || (snap[i].state != NMADJ_CLAIMED && snap[i].state != NMADJ_PUBLISHED))
			continue;
		if (NmJobsConflict(me.d, snap[i].d))
		{
			viol = true;
			InterlockedIncrement(&s_viol);
			ViolLine("job", me, snap[i], -1000, 0);
		}
	}

	if (splice)
		return;   // two private stack instances
	// Object side: an exterior this stitch touches that belongs to another live
	// exterior job.
	const void* sides[2] = { a, b };
	for (int s = 0; s < 2; ++s)
	{
		int uid = -1;
		if (!ReadUid(sides[s], &uid) || uid < 0 || (unsigned int)uid >= NMADJ_INTERIOR_UID_MIN)
			continue;
		const int cx = uid & 0xFF, cy = (uid >> 8) & 0xFF;
		if (me.d.kind != NMADJ_KIND_E && !NmAdjReaches(me.d, cx, cy))
			InterlockedIncrement(&s_spanOut);
		for (int i = 0; i < high; ++i)
		{
			if (i == own || (snap[i].state != NMADJ_CLAIMED && snap[i].state != NMADJ_PUBLISHED))
				continue;
			if (snap[i].d.kind == NMADJ_KIND_E && snap[i].d.x == cx && snap[i].d.y == cy)
			{
				InterlockedIncrement(&s_violCell);
				ViolLine("cell", me, snap[i], cx, cy);
				break;
			}
		}
	}
}
