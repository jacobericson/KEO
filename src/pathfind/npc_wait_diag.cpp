// npc_wait_diag.cpp - bounded NPC path-wait walk.
// Main thread once per second; the guarded walk keeps its two __try bodies whole.

#include "pathfind/path_pool_internal.h"

namespace npc_wait_diag_detail {
struct NpcWaitEntry
{
	void*  hc;
	int    state;
	double since;      // when 'state' was last observed changing
	float  posX, posZ; // last CharMovement position sample
	double posSince;   // when the position last moved past NPC_NO_MOVE_EPS_SQ
	bool   inUse;
};

struct NpcWaitWalkResult
{
	int waitingTotal, waiting4, waiting5;
	int failed3FarDest;
	int stoppedFarDestNoMove;
	int navWaitZoneNotReady;
	double longestWaitSec;
	int haveTop;
	float topPosX, topPosZ;
	int topGx, topGy;
	int topState;
	double topDestDist;
	int topZoneReady;      // -1 unknown, 0/1
	int playerWaitingTotal, playerWaiting4, playerWaiting5;
	// 1 when the walk faulted part-way (a node or character freed mid-walk):
	// the counts above cover only the characters seen before the fault, and
	// the NpcPathWait line says so (partial=1) instead of passing them off as
	// the whole list.
	int faulted;
};
} // namespace npc_wait_diag_detail
using namespace npc_wait_diag_detail;

namespace path_pool_detail {
// =========================================================================
// NPC wait diagnostic (main thread, once per second)
// =========================================================================
//
// Walks pauseState.charUpdateListMain (a boost::unordered_set<Character*>;
// same layout as ZoneManager's Set B, OFF_SET_* in game.h). The set is
// mutated by the game (character creation/destruction), so the whole walk
// runs inside one guarded, POD-only, standalone function -- MSVC 2010 forbids
// __try in a function with objects needing unwinding, and a fault here (a
// node freed mid-walk) must end the walk, not reach the crash recorder
// (core.h's GuardEnter/GuardLeave contract), mirroring destroy_list_defer.cpp's
// ReadContainer/ProbeContainer pattern.

static const unsigned __int64 PP_MAX_SANE_BUCKETS = 0x100000;
static const int   NPC_WAIT_TABLE_SIZE = 4096;
static const int   NPC_WAIT_WALK_CAP   = 16384;   // defensive bound on list length
static const float NPC_FAR_DEST_UNITS  = 100.0f;
static const float NPC_NO_MOVE_EPS_SQ  = 0.25f;   // 0.5 units
static const double NPC_NO_MOVE_SEC    = 5.0;



static NpcWaitEntry g_npcWaitTable[NPC_WAIT_TABLE_SIZE];

static PPHist g_finishedWaitHist;      // seconds*1e6, waits that ended this window
// Character-samples in state 6 across this window's once-per-second polls
// (NOT a count of polls: every character seen in state 6 on a given poll
// adds one sample, so N characters stuck in state 6 on the same poll add N).
// Split by player-owned so the NPC line's reissue(6) only counts NPCs.
static volatile LONG g_reissueSamples       = 0;  // non-player
static volatile LONG g_reissueSamplesPlayer = 0;  // player-owned

static uintptr_t g_npcZmSeen     = 0;
static bool      g_npcWasLoading = false;

static int NpcWaitHash(void* hc)
{
	unsigned __int64 v = (unsigned __int64)hc;
	v ^= v >> 15;
	v *= 0x2545F4914F6CDD1DULL;
	v ^= v >> 32;
	return (int)(v % (unsigned __int64)NPC_WAIT_TABLE_SIZE);
}

static void NpcWaitTableReset()
{
	memset(g_npcWaitTable, 0, sizeof(g_npcWaitTable));
	PPHistReset(&g_finishedWaitHist);
	InterlockedExchange(&g_reissueSamples, 0);
	InterlockedExchange(&g_reissueSamplesPlayer, 0);
}

// Standalone, POD-only (MSVC 2010 __try rule). 'zoneMgr' may be NULL (no
// zone(gx,gy)/zoneReady lookups then). Everything read here is a raw offset
// off a game pointer; a fault (the set or a character being torn down mid
// walk) unwinds only this function and the walk stops where it is.
static void PPWalkCharList(void* head, double now, void* zoneMgr, NpcWaitWalkResult* r)
{
	memset(r, 0, sizeof(*r));
	r->topZoneReady = -1;

	int seen = 0;
	GuardEnter();
	__try
	{
		void* node = head;
		while (node && seen < NPC_WAIT_WALK_CAP)
		{
			void* character = *(void**)(KLIB_MEMBER(5, (char*)node, CharacterSetNode_value_base_, OFF_SET_NODE_VALUE));
			void* nextNode  = *(void**)KLIB_MEMBER(5, node, CharacterSetNode_next_, 0);
			++seen;

			if (!character)
			{
				node = nextNode;
				continue;
			}

			void* cm = *(void**)(KLIB_MEMBER(5, (char*)character, Character_movement, OFF_CHAR_MOVEMENT));
			if (!cm) { node = nextNode; continue; }
			void* hc = *(void**)(KLIB_MEMBER(5, (char*)cm, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
			if (!hc) { node = nextNode; continue; }

			int state = *(int*)(KLIB_MEMBER(5, (char*)hc, HavokCharacter_pathState, OFF_HC_PATH_STATE));

			float posX = *(float*)(KLIB_MEMBER(5, (char*)cm, AbstractMovementBase_pos_x, OFF_CMOV_POS));
			float posZ = *(float*)(KLIB_MEMBER(5, (char*)cm, AbstractMovementBase_pos_z, OFF_CMOV_POS + 8));
			float destX = *(float*)(KLIB_MEMBER(5, (char*)cm, AbstractMovementBase_destination_x, OFF_CMOV_LAST_DEST));
			float destZ = *(float*)(KLIB_MEMBER(5, (char*)cm, AbstractMovementBase_destination_z, OFF_CMOV_LAST_DEST + 8));

			float ddx = destX - posX, ddz = destZ - posZ;
			double destDist = sqrt((double)(ddx * ddx + ddz * ddz));
			bool farDest = destDist > NPC_FAR_DEST_UNITS;

			bool isPlayer = fn_isPriorityPath && fn_isPriorityPath(character);

			int idx = NpcWaitHash(hc);
			NpcWaitEntry& e = g_npcWaitTable[idx];

			if (!e.inUse || e.hc != hc)
			{
				// New slot or collision: reset tracking for this hc (an
				// approximation under the fixed 4096-slot table -- a
				// collision loses the evicted character's "since" history
				// for this window, which is acceptable for a diagnostic).
				e.hc = hc;
				e.inUse = true;
				e.state = state;
				e.since = now;
				e.posX = posX; e.posZ = posZ; e.posSince = now;
			}
			else
			{
				if (e.state != state)
				{
					if ((e.state == 4 || e.state == 5) && !(state == 4 || state == 5))
					{
						double dur = now - e.since;
						if (dur < 0) dur = 0;
						PPHistAdd(&g_finishedWaitHist, (LONGLONG)(dur * 1000000.0));
					}
					e.state = state;
					e.since = now;
				}
				float dx = posX - e.posX, dz = posZ - e.posZ;
				if (dx * dx + dz * dz > NPC_NO_MOVE_EPS_SQ)
				{
					e.posX = posX; e.posZ = posZ; e.posSince = now;
				}
			}

			bool navWait = false;
			if ((state == 0 || state == 1) && zoneMgr)
			{
				int gx = -1, gy = -1;
				if (WorldToZoneGrid(posX, posZ, &gx, &gy))
				{
					void* zone = GetZoneEntry(zoneMgr, gx, gy);
					if (zone && *(unsigned char*)(KLIB_MEMBER(5, (char*)zone, ZoneMap_stateT_mainThreadData__zoneIsLoaded, OFF_ZONE_IS_ACCESS)) == 0)
						navWait = true;
				}
			}

			if (!isPlayer)
			{
				if (state == 6)
					InterlockedIncrement(&g_reissueSamples);
				if (state == 4 || state == 5)
				{
					r->waitingTotal++;
					if (state == 4) r->waiting4++; else r->waiting5++;
					double waitSec = now - e.since;
					if (waitSec > r->longestWaitSec)
					{
						r->longestWaitSec = waitSec;
						r->haveTop = 1;
						r->topPosX = posX; r->topPosZ = posZ;
						r->topState = state;
						r->topDestDist = destDist;
						int gx = -1, gy = -1;
						if (WorldToZoneGrid(posX, posZ, &gx, &gy))
						{
							r->topGx = gx; r->topGy = gy;
							void* zone = zoneMgr ? GetZoneEntry(zoneMgr, gx, gy) : NULL;
							if (zone)
								r->topZoneReady = (*(unsigned char*)(KLIB_MEMBER(5, (char*)zone, ZoneMap_stateT_mainThreadData__zoneIsLoaded, OFF_ZONE_IS_ACCESS)) != 0) ? 1 : 0;
						}
					}
				}
				if (state == 3 && farDest)
					r->failed3FarDest++;
				if ((state == 0 || state == 1) && farDest && (now - e.posSince) >= NPC_NO_MOVE_SEC)
					r->stoppedFarDestNoMove++;
				if (navWait)
					r->navWaitZoneNotReady++;
			}
			else
			{
				if (state == 6)
					InterlockedIncrement(&g_reissueSamplesPlayer);
				if (state == 4 || state == 5)
				{
					r->playerWaitingTotal++;
					if (state == 4) r->playerWaiting4++; else r->playerWaiting5++;
				}
			}

			node = nextNode;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		r->faulted = 1;
	}
	GuardLeave();
}

// Standalone, POD-only, mirrors destroy_list_defer.cpp's ReadContainer (MSVC 2010 __try
// rule). Reads the boost::unordered_set head at pauseState+0x750.
static bool PPReadCharListHead(uintptr_t base, void** headOut)
{
	bool ok = true;
	*headOut = NULL;
	GuardEnter();
	__try
	{
		unsigned __int64 nbuckets = *(unsigned __int64*)(KLIB_MEMBER(5, base, CharacterSetTable_bucket_count_, OFF_SET_BUCKET_COUNT));
		void** buckets = *(void***)(KLIB_MEMBER(5, base, CharacterSetTable_buckets_, OFF_SET_BUCKETS));
		if (buckets && nbuckets < PP_MAX_SANE_BUCKETS)
			*headOut = buckets[nbuckets];
		else if (nbuckets >= PP_MAX_SANE_BUCKETS)
			ok = false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// pauseState.charUpdateListMain: pauseState+0x750 (IDA-confirmed, 2
// references, one of them consumePathResults' own sentinel-branch walk of
// the same list). pauseState itself is RVA_GLOBAL_GAMEWORLD (rva.h).
static const size_t OFF_PAUSESTATE_CHAR_UPDATE_LIST_MAIN = 0x750;

static NpcWaitWalkResult g_lastWalk;
static bool              g_haveLastWalk = false;

void RunNpcWaitDiagnostic(double now)
{
	void* zoneMgr = g_cachedZoneMgr;

	// Save-load / new ZoneManager reset, mirroring IslandTick's save-load reset (islands.cpp)
	// (ZM+8 rising edge or a new ZoneManager pointer) without including
	// islands.h.
	uintptr_t zm = (uintptr_t)zoneMgr;
	bool loading = false;
	if (zm)
	{
		loading = *(unsigned char*)(KLIB_MEMBER(5, zm, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0;
		if (zm != g_npcZmSeen || (loading && !g_npcWasLoading))
		{
			NpcWaitTableReset();
			g_npcZmSeen = zm;
			g_haveLastWalk = false;
		}
		g_npcWasLoading = loading;
	}

	// Skip the walk itself while loading (ZM+8 set), same as IslandTick's gate
	// around WalkSetB (island_components.cpp) -- the character list and
	// CharMovement/HavokCharacter fields it reads are least stable exactly
	// while a save or zone load is in progress.
	if (loading)
		return;

	if (!gameBase)
		return;

	void* head = NULL;
	if (!PPReadCharListHead(KLIB_MEMBER(5, (uintptr_t)GameAddr(RVA_GLOBAL_GAMEWORLD), GameWorld_charUpdateListMain, OFF_PAUSESTATE_CHAR_UPDATE_LIST_MAIN), &head))
		return;
	if (!head)
		return;

	NpcWaitWalkResult r;
	PPWalkCharList(head, now, zoneMgr, &r);
	g_lastWalk = r;
	g_haveLastWalk = true;
}


void PrintNpcPathWaitLine(double windowSec)
{
	if (!g_haveLastWalk)
		return;

	NpcWaitWalkResult r = g_lastWalk;

	double finP50 = PPHistPercentileUs(&g_finishedWaitHist, 0.50) / 1000000.0;
	double finP90 = PPHistPercentileUs(&g_finishedWaitHist, 0.90) / 1000000.0;
	double finMax = InterlockedExchange(&g_finishedWaitHist.maxUs, 0) / 1000000.0;
	PPHistReset(&g_finishedWaitHist);

	LONG reissueSamples       = InterlockedExchange(&g_reissueSamples, 0);
	LONG reissueSamplesPlayer = InterlockedExchange(&g_reissueSamplesPlayer, 0);
	// reissue(6) is character-samples in state 6 across this window's
	// once-per-second polls (not a poll count: N characters seen in state 6
	// on one poll add N, not 1), normalised to a /10s rate using windowSec
	// (measured by PathPoolTickMain from the real interval between prints,
	// so this is correct in both DEV's 10s and PROD's 30s window, and on a
	// shorter first window). Split by player-owned, so this rate counts
	// only NPCs.
	double reissueRate10s       = (windowSec > 0.0) ? ((double)reissueSamples * 10.0 / windowSec) : 0.0;
	double reissueRate10sPlayer = (windowSec > 0.0) ? ((double)reissueSamplesPlayer * 10.0 / windowSec) : 0.0;

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "NpcPathWait: waiting=" << r.waitingTotal
	   << " (4:" << r.waiting4 << " 5:" << r.waiting5 << ")"
	   << " finished p50/p90/max=" << finP50 << "/" << finP90 << "/" << finMax << "s"
	   << " longest=" << r.longestWaitSec << "s\n"
	   << "  failed(3,farDest)=" << r.failed3FarDest
	   << " reissue(6)=" << reissueRate10s << "/10s"
	   << " stopped(farDest,st0/1,noMove5s)=" << r.stoppedFarDestNoMove
	   << " navWait(st0/1,zoneNotReady)=" << r.navWaitZoneNotReady;
	if (r.faulted)
		ss << " partial=1";
	if (r.haveTop)
	{
		ss << "\n  top: (" << r.topPosX << "," << r.topPosZ << ")"
		   << " zone(" << r.topGx << "," << r.topGy << ")"
		   << " state=" << r.topState
		   << " destDist=" << r.topDestDist
		   << " zoneReady=" << r.topZoneReady;
	}
	ss << "\n  player: waiting=" << r.playerWaitingTotal
	   << " (4:" << r.playerWaiting4 << " 5:" << r.playerWaiting5 << ")"
	   << " reissue(6)=" << reissueRate10sPlayer << "/10s";
	LogMsg(ss.str());
}


} // namespace path_pool_detail
