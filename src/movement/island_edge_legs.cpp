#include "movement/island_edge_legs.h"


#include "game/game.h"
#include "movement/islands.h"
#include "pathfind/pathfind_diag.h"
#include "movement/island_span_policy.h"
#include "movement/island_edge_ring_policy.h"
#include "zone/grid.h"   // zoneStepX: cell size for the legLen= bucket
#include <math.h>
#include <string.h>
#include <iomanip>

namespace island_edge_legs_detail {

const int    WOULD_SPAN      = 2;      // the order class a control run reports
const float  LEG_MOVE_DIST   = 1.0f;   // pathDestination is written only by a request
const double PARK_HOLD_SEC   = 3.0;    // a park must hold this long to count
const int    LADDER_EXHAUSTED = 17;    // setDestination's road fallback rung

struct EdgeLegSlot
{
	bool   inSet;          // this order satisfied the predicate at some frame
	bool   edgeSeen;
	bool   edgePrev;
	bool   arrivedPrev;
	bool   haveLeg;
	float  wpX, wpZ;       // pathDestination at the current leg's start
	float  legStartD;      // distance to the order's destination then
	int    legs;           // path requests while in edge mode
	int    rungs;          // of those, retry-ladder rungs (edgeTarget rose)
	int    ladderPrev;
	float  progress;       // sum of per-leg reductions in that distance
	int    backLegs;       // legs that started farther away than the previous one
	int    ladderMax;
	bool   ladderOut;
	int    parkKind;       // IslandEdgePark of the running episode
	double parkSince;
	bool   parkCounted;
	int    parks[ISLAND_EDGE_PARK_KINDS];   // counted episodes per IslandEdgePark
};

EdgeLegSlot g_slots[MAX_TRACKED_PLAYERS];

// Session totals over in-set orders.
long  g_ordSet = 0, g_ordEdge = 0;
long  g_legs = 0, g_rungs = 0, g_backLegs = 0;
float g_progress = 0.0f;
long  g_parks[ISLAND_EDGE_PARK_KINDS] = { 0 };
int   g_ladderMax = 0;
long  g_ladderOut = 0;
long  g_handover = 0, g_edgeDrop = 0;
long  g_arrNear = 0, g_arrFar = 0;
long  g_legLen[ISLAND_LEGLEN_BUCKETS] = { 0 };   // leg length in cells at leg start

int Threshold() { return IslandFarSpanArmed() ? movement::g_movementCfg.cfg_islandFarSpan : WOULD_SPAN; }

} // namespace
using namespace island_edge_legs_detail;


void EdgeLegsForget(int slot)
{
	if (slot < 0 || slot >= MAX_TRACKED_PLAYERS) return;
	memset(&g_slots[slot], 0, sizeof(g_slots[slot]));
}

void EdgeLegsSample(int slot, uintptr_t cm, int hc136, bool moving, bool edge,
                    float posX, float posZ, float destX, float destZ, double now)
{
	if (slot < 0 || slot >= MAX_TRACKED_PLAYERS || !cm || !g_cachedZoneMgr) return;
	EdgeLegSlot& e = g_slots[slot];

	// The rule's own inputs for this character: the same labels and span the
	// far-arrival latch reads, evaluated against the same predicate.
	int sl = 0, dl = 0, span = -1;
	bool haveSpan = IslandSampleLabels(g_cachedZoneMgr, posX, posZ, destX, destZ, &sl, &dl, &span);
	bool inRule = haveSpan && IslandFarSpanFlips(sl == dl, sl, span, Threshold());
	if (inRule && !e.inSet)
	{
		e.inSet = true;
		g_ordSet++;
	}
	if (!e.inSet)
		return;

	float dx = destX - posX, dz = destZ - posZ;
	float destD = sqrtf(dx * dx + dz * dz);
	float wpX = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	float wpZ = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	int ladder = *(int*)(KLIB_MEMBER(3, cm, CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER));

	if (edge)
	{
		if (!e.edgeSeen) { e.edgeSeen = true; g_ordEdge++; }

		float mx = wpX - e.wpX, mz = wpZ - e.wpZ;
		if (!e.edgePrev || !e.haveLeg || mx * mx + mz * mz > LEG_MOVE_DIST * LEG_MOVE_DIST)
		{
			if (e.haveLeg)
			{
				float gained = e.legStartD - destD;
				e.progress += gained;
				g_progress += gained;
				if (gained < 0.0f) { e.backLegs++; g_backLegs++; }
			}
			e.haveLeg = true;
			e.wpX = wpX; e.wpZ = wpZ;
			e.legStartD = destD;
			e.legs++;
			g_legs++;
			{
				float ldx = wpX - posX, ldz = wpZ - posZ;
				g_legLen[IslandClassifyLegLen(sqrtf(ldx * ldx + ldz * ldz), zoneStepX)]++;
			}
			if (e.edgePrev && ladder > e.ladderPrev) { e.rungs++; g_rungs++; }
		}
		e.ladderPrev = ladder;

		if (ladder > e.ladderMax) e.ladderMax = ladder;
		if (ladder > g_ladderMax) g_ladderMax = ladder;
		if (ladder >= LADDER_EXHAUSTED && !e.ladderOut) { e.ladderOut = true; g_ladderOut++; }
	}
	else if (e.edgePrev)
	{
		// Leaving edge mode below the threshold is the designed handover to a
		// direct path; anywhere else it was abandoned (a new order, a halt).
		if (haveSpan && span < Threshold()) g_handover++;
		else                                g_edgeDrop++;
	}
	e.edgePrev = edge;

	float px = wpX - posX, pz = wpZ - posZ;
	float lx = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST)) - posX;
	float lz = *(float*)(KLIB_MEMBER(3, cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8)) - posZ;
	bool idle = hc136 <= 1 && !moving;
	int kind = IslandClassifyEdgePark(edge, idle, sqrtf(lx * lx + lz * lz),
	                                  sqrtf(px * px + pz * pz), destD);
	if (kind != e.parkKind)
	{
		e.parkKind = kind;
		e.parkSince = now;
		e.parkCounted = false;
	}
	if (kind != EDGEPARK_NONE && !e.parkCounted && now - e.parkSince >= PARK_HOLD_SEC)
	{
		e.parkCounted = true;
		e.parks[kind]++;
		g_parks[kind]++;
	}

	bool arrived = hc136 == 1 && !moving && !edge;
	if (arrived && !e.arrivedPrev)
	{
		if (destD > 100.0f) g_arrFar++;
		else                g_arrNear++;
	}
	e.arrivedPrev = arrived;
}

void EdgeLegsAppendStuck(int slot, std::ostringstream& ss)
{
	if (slot < 0 || slot >= MAX_TRACKED_PLAYERS) return;
	const EdgeLegSlot& e = g_slots[slot];
	ss << " far=" << (IslandFarSpanArmed() ? "flip" : "would") << (e.inSet ? 1 : 0);
	if (!e.inSet) return;
	ss << " legs=" << e.legs
	   << "/rung" << e.rungs
	   << "/prog" << (long)e.progress
	   << "/back" << e.backLegs
	   << " park=e" << e.parks[EDGEPARK_AT_EDGE] << "/s" << e.parks[EDGEPARK_SHORT_LEG]
	   << "/h" << e.parks[EDGEPARK_HALTED]
	   << " ladder=" << e.ladderMax;
}

void EdgeLegsAppendSummary(std::ostringstream& ss)
{
	ss << " legs(" << (IslandFarSpanArmed() ? "flip" : "would") << Threshold() << "):"
	   << " ord=" << g_ordSet
	   << " edgeIn=" << g_ordEdge
	   << " legs=" << g_legs
	   << " rungs=" << g_rungs
	   << " prog=" << (long)g_progress
	   << " back=" << g_backLegs
	   << " park=e" << g_parks[EDGEPARK_AT_EDGE] << "/s" << g_parks[EDGEPARK_SHORT_LEG]
	   << "/h" << g_parks[EDGEPARK_HALTED]
	   << " ladder=" << g_ladderMax << "/out" << g_ladderOut
	   << " handover=" << g_handover
	   << " drop=" << g_edgeDrop
	   << " arr=near" << g_arrNear << "/far" << g_arrFar
	   << " legLen=<1:" << g_legLen[LEGLEN_LT1]
	   << "/1-2:" << g_legLen[LEGLEN_1TO2]
	   << "/2-3:" << g_legLen[LEGLEN_2TO3]
	   << "/3+:" << g_legLen[LEGLEN_3PLUS] << ">";
}

