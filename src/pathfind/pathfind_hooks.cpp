// pathfind_hooks.cpp -- the pathfinding hooks
//
// csFindPath (count, probe), csCheckFaceConn (count, bypass: return 1, probe),
// findPathFull (count, status, probe, hierarchical arm), requestPath (player/NPC count, formation
// match, tier), pathReqSubmit (priority override), csFindPathFallback
// (reachability count).

#include "pathfind/pathfind_diag.h"
#include "pathfind/pathfind_cache.h"
#include "movement/formation.h"
#include "movement/tracking.h"
#include "pathfind/path_pool.h"
#include "pathfind/astar_cost.h"
#include "pathfind/astar_hier.h"
#include "pathfind/player_repath_tier.h"
#include "pathfind/player_repath_tier_policy.h"
#include "fixes/streaming/navmesh_life.h"
#include "fixes/stitch/stitch_source.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)


// Extraction guard + streaming-collection timestamp counters
static volatile long extractionCrashRescue = 0;  // faults caught inside the extraction loop
static volatile long addInstanceHookCalls  = 0;  // sanity: the addInstance hook is firing

// Request pointer recovered by hook_csFindPath, for whichever request
// hook_findPathFull runs next on this thread, so its PathSearchSample can
// carry playerByReq (*(int*)(req+0x2C) >= 20). The dispatcher passes req+128
// as resultBuf (OFF_REQ_RESULTBUF_SLOT, game.h).
// Thread-local for the same reason as currentRequestIsPlayer below: findPathFull has 6 callers
// and only the csFindPath -> fallback -> findPathFull chain shares a thread.
// hook_findPathFull consumes and clears it, so a call that did not come
// through hook_csFindPath first (the gate pass calls findPathFull directly)
// reports playerByReq = -1.
static __declspec(thread) void* g_pathPoolLastCsFindPathReq = NULL;

// BG thread tag: set by hook_csFindPath, read by hook_findPathFull.
// Thread-local: findPathFull has 6 callers; only the csFindPathFallback chain runs
// on the same contentStream bg thread as csFindPath. Game-internal findPathFull calls
// on other bg threads would otherwise see stale flags from another thread.
static __declspec(thread) bool currentRequestIsPlayer = false;


// =========================================================================
// Hook 1: ContentStream::findPath (primary "direct path" check)
// =========================================================================
// RVA 0x3AA950. Called for every path request. Returns non-zero on success.
// Runs on contentStream bg thread.

char hook_csFindPath(void* manager, unsigned int startFaceKey, void* startPos,
                      void* destPos, float radius, char param5, void* resultBuf)
{
	InterlockedIncrement(&pathfind::g_pathDiag.diagPrimaryAttempts);

	char result = game::g_hookOrig.orig_csFindPath(manager, startFaceKey, startPos, destPos,
	                               radius, param5, resultBuf);

	if (result)
		InterlockedIncrement(&pathfind::g_pathDiag.diagPrimarySuccess);
	else
		InterlockedIncrement(&pathfind::g_pathDiag.diagPrimaryFail);

	// Tag this request for whichever hook_findPathFull call runs next on
	// this thread (playerByReq). Set on every call, success or
	// failure -- on success no findPathFull call follows for this request, so
	// the tag is simply overwritten by the next request's csFindPath call (or
	// consumed and cleared by an unrelated findPathFull call in between).
	// The game passes req+128 as resultBuf.
	g_pathPoolLastCsFindPathReq = resultBuf ? (void*)((char*)resultBuf - OFF_REQ_RESULTBUF_SLOT) : NULL;


	// Player request tagging: boosted requests (pri 45) sort to top of queue.
	// Decrement counter to tag sequential requests as player-owned.
	currentRequestIsPlayer = false;
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.playerRequestsInFlight, 0, 0) > 0)
	{
		InterlockedDecrement(&pathfind::g_pathDiag.playerRequestsInFlight);
		currentRequestIsPlayer = true;
	}

	// Multi-call probe: capture csFindPath positions when armed
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.pathProbeArmed, 0, 0) == 1)
	{
		long idx = InterlockedIncrement(&pathfind::g_pathDiag.pathProbeWriteIdx) - 1;
		if (idx < PATH_PROBE_SIZE)
		{
			float* sp = (float*)startPos;
			float* dp = (float*)destPos;
			pathfind::g_pathDiag.pathProbeBuf[idx].startX = (*(const float*)KLIB_MEMBER(5, sp, hkVector4f_x, 0));
			pathfind::g_pathDiag.pathProbeBuf[idx].startY = (*(const float*)KLIB_MEMBER(5, sp, hkVector4f_y, 4));
			pathfind::g_pathDiag.pathProbeBuf[idx].startZ = (*(const float*)KLIB_MEMBER(5, sp, hkVector4f_z, 8));
			pathfind::g_pathDiag.pathProbeBuf[idx].destX  = (*(const float*)KLIB_MEMBER(5, dp, hkVector4f_x, 0));
			pathfind::g_pathDiag.pathProbeBuf[idx].destY  = (*(const float*)KLIB_MEMBER(5, dp, hkVector4f_y, 4));
			pathfind::g_pathDiag.pathProbeBuf[idx].destZ  = (*(const float*)KLIB_MEMBER(5, dp, hkVector4f_z, 8));
			pathfind::g_pathDiag.pathProbeBuf[idx].hookType = 1;
			pathfind::g_pathDiag.pathProbeBuf[idx].result   = result ? 1 : 0;
			pathfind::g_pathDiag.pathProbeBuf[idx].faceKey  = startFaceKey;
		}
	}

	return result;
}


// =========================================================================
// Hook 2: ContentStream::checkFaceConnectivity
// =========================================================================
// RVA 0x3A5B00. Called when primary findPath fails. Returns true if start
// and dest faces are in connected navmesh sections.
// Runs on contentStream bg thread.

// The multi-call probe's row for this hook: no positions, the destination key
// and the answer the caller receives.
static void RecordConnProbe(unsigned int destFace, char answer)
{
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.pathProbeArmed, 0, 0) != 1)
		return;
	long idx = InterlockedIncrement(&pathfind::g_pathDiag.pathProbeWriteIdx) - 1;
	if (idx >= PATH_PROBE_SIZE)
		return;
	pathfind::g_pathDiag.pathProbeBuf[idx].startX = 0; pathfind::g_pathDiag.pathProbeBuf[idx].startY = 0; pathfind::g_pathDiag.pathProbeBuf[idx].startZ = 0;
	pathfind::g_pathDiag.pathProbeBuf[idx].destX  = 0; pathfind::g_pathDiag.pathProbeBuf[idx].destY  = 0; pathfind::g_pathDiag.pathProbeBuf[idx].destZ  = 0;
	pathfind::g_pathDiag.pathProbeBuf[idx].hookType = 3;
	pathfind::g_pathDiag.pathProbeBuf[idx].result   = answer ? 1 : 0;
	pathfind::g_pathDiag.pathProbeBuf[idx].faceKey  = destFace;
}

// A pair the cluster graph rejected and the mod waved through anyway, waiting
// for the search it gated. The engine runs the two back to back on one thread
// -- the connectivity call guards the findPath call a few instructions later,
// and that findPath reaches the A* unconditionally -- so the next search on
// this thread is that pair's search.
//
// Thread-local, because the same connectivity call also serves a query that
// never searches at all. On a thread that never consumes a tag, one simply
// replaces the last; t_waveConsumed keeps that case out of the leak count, so
// waveStale counts only the tags a searching thread really dropped.
static __declspec(thread) int t_waveTag      = 0;
static __declspec(thread) int t_waveConsumed = 0;

// The engine's own verdict on the request this thread is serving, taken from
// the argument NavMesh::findPath receives (0 = NPC, 1 = npc label, 2 = player
// label). Set by hook_csFindPathFallback, which wraps that call, and consumed
// by the search inside it. 0 means no verdict was seen, which is a fact about
// the measurement and never a side.
static __declspec(thread) int t_waveAuth = 0;

// Who the connectivity check is being asked on behalf of, read at the check
// itself. The direct-path check runs a few instructions earlier on this same
// thread for this same request and leaves the request pointer behind; the
// queue priority in it (>= 20 is player-owned) is the only requester label
// available this early, because the engine does not resolve the character
// until after the check has answered.
//
// Read-only: the attribution in hook_findPathFull still consumes the pointer.
// A thread that never runs the direct-path check -- the AI task system's
// reachability query -- has none here and gets WAVE_UNK.
static int ConnGateOwner()
{
	if (!g_pathPoolLastCsFindPathReq)
		return WAVE_UNK;
	return (*(int*)((uintptr_t)g_pathPoolLastCsFindPathReq + 0x2C) >= 20)
		? WAVE_PLAYER : WAVE_NPC;
}

char hook_csCheckFaceConn(void* manager, unsigned int startFace, unsigned int destFace)
{
	InterlockedIncrement(&pathfind::g_pathDiag.diagConnAttempts);

	// Bypass the cluster graph connectivity pre-check entirely.
	// NPCs going through cluster-graph traversal hit sub_140DA4470 which derefs
	// m_instances[sec]+16 and races with addInstance (observed crash at 0xDA44D5).
	// The bypass closes that vector at the cost of letting every pair through to
	// the A* search; clusterGraphBypass=false gives the graph its say back.
	if (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_BYPASS)
	{
		RecordConnProbe(destFace, 1);
		return 1;
	}

	// Resolved before the original runs, so the label belongs to the request
	// that opened this check and not to anything the call itself starts.
	int gateOwner = WAVE_UNK;
	if (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_MEASURE || pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_PLAYER)
		gateOwner = ConnGateOwner();

	char result = game::g_hookOrig.orig_csCheckFaceConn(manager, startFace, destFace);
	if (!result)
	{
		InterlockedIncrement(&pathfind::g_pathDiag.diagConnFail);
		if (currentRequestIsPlayer)
		{
			long idx = InterlockedIncrement(&pathfind::g_pathDiag.playerFailWriteIdx) - 1;
			int slot = (int)(idx % PLAYER_FAIL_RING);
			pathfind::g_pathDiag.playerFailRing[slot].goalX = 0;
			pathfind::g_pathDiag.playerFailRing[slot].goalZ = 0;
			InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].status, 99);  // 99 = connectivity rejection
			InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].cause, 0);
			InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].iterCount, 0);
			InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].valid, 1);
		}
	}
	RecordConnProbe(destFace, result);
	// The graph answered. Measure records the answer and waves every rejected
	// pair through, so routing matches the plain bypass while each waved
	// search is labelled. Player waves only the pairs a player request asked
	// about and lets the rest of the rejections stand.
	//
	// A pair the graph accepts takes the same route in both, so only the
	// rejection needs deciding. An unattributed rejection is obeyed: the
	// unknown case is the one vanilla already handles.
	if (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_MEASURE || pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_PLAYER)
	{
		if (!result)
		{
			InterlockedIncrement(&pathfind::g_pathDiag.diagConnRejectByGate[gateOwner]);
			if (pathfind::g_pathfindCfg.clusterGraphBypassMode == CGB_PLAYER && gateOwner != WAVE_PLAYER)
				return 0;
			if (t_waveTag && t_waveConsumed)
				InterlockedIncrement(&pathfind::g_pathDiag.diagWaveStale);
			t_waveTag = 1;
		}
		return 1;
	}
	return result;
}


// =========================================================================
// Hook 3: Havok::findPathFull (hkaiPathfindingUtil::findPath)
// =========================================================================
// RVA 0xCE56D0. Full A* search. Output: +48=iterations, +60=status, +61=cause.
// Runs on contentStream bg thread.

namespace pathfind_hooks_detail
{

// POD state for one invocation; the one-shot TLS labels are taken before orig can run a nested search.
struct FindPathFullCtx
{
	void* astarReturnAddr;
	bool wavedThrough;
	int pathPoolPlayerByReq;
	AstarCallerClass astarCallerClass;
	unsigned astarStartFaceKey, astarGoalFaceKey;
	float astarGoalDist3D;
	int waveOwner, waveGateIdx, waveTagIdx;
	int pathPoolBoosted;
	LARGE_INTEGER pathPoolQpcBefore;
	long probeSlot;
	unsigned char status, cause;
	int iterCount;
	LARGE_INTEGER pathPoolQpcAfter;
};

static void TakeFindPathWave(FindPathFullCtx& c)
{
	// Take the waved-through label, if this thread is carrying one, before
	// anything below can run a nested search that would inherit it. The
	// counting happens a few lines down, once the request this search belongs
	// to has been resolved, but still ahead of the original, so a waved search
	// that faults shows up as a start with no outcome.
	c.wavedThrough = false;
	if (t_waveTag)
	{
		t_waveTag = 0;
		t_waveConsumed = 1;
		c.wavedThrough = true;
	}
}

static void ResolveFindPathRequest(FindPathFullCtx& c)
{
	// Resolve playerByReq once from the request hook_csFindPath tagged.
	// Clear it so an unrelated direct call reports -1. The hook separately
	// tracks the budget boost and takes the "before" QPC immediately ahead
	// of its single original call. Status, cause and iterations are read
	// before the "after" QPC, then RecordFindPathSamples hands the record
	// to PathPoolNoteSearch and AstarCostNote once for this invocation.
	// The sample's playerByTag and the failure-ring predicate still read
	// currentRequestIsPlayer after the original; no allocation or logging.
	c.pathPoolPlayerByReq = -1;
	if (g_pathPoolLastCsFindPathReq) {
		c.pathPoolPlayerByReq = (*(int*)((uintptr_t)g_pathPoolLastCsFindPathReq + 0x2C) >= 20) ? 1 : 0;
		g_pathPoolLastCsFindPathReq = NULL;
	}
}

static void ReadFindPathInput(void* searchState, FindPathFullCtx& c)
{
	// Classify the caller once (return address + the request-derived tag
	// just resolved above), and read the input fields the per-search record
	// needs, before orig can touch searchState. One sample, two consumers:
	// this class value and these fields feed both PathSearchSample.callerClass
	// (path_pool_hist.cpp's PathBusy split) and AstarCostSample (astar_cost.cpp's
	// histograms/cap counters) below.
	c.astarCallerClass = AstarResolveCallerClass(c.astarReturnAddr, c.pathPoolPlayerByReq);

	c.astarStartFaceKey = 0xFFFFFFFFu;
	c.astarGoalFaceKey  = 0xFFFFFFFFu;
	c.astarGoalDist3D   = -1.0f;
	if (searchState)
	{
		uintptr_t ss = (uintptr_t)searchState;
		c.astarStartFaceKey = *(unsigned*)(ss + 48);   // +0x30

		unsigned* goalKeys = *(unsigned**)(ss + 56); // +0x38
		int goalKeyCount   = *(int*)(ss + 64);       // +0x40
		if (goalKeys && goalKeyCount >= 1)
			c.astarGoalFaceKey = goalKeys[0];

		float* startPos    = (float*)(ss + 16);       // +0x10
		float* goalPos     = *(float**)(ss + 32);     // +0x20
		int    goalPosCount = *(int*)(ss + 40);       // +0x28
		if (goalPos && goalPosCount >= 1)
		{
			float dx = goalPos[0] - startPos[0];
			float dy = goalPos[1] - startPos[1];
			float dz = goalPos[2] - startPos[2];
			c.astarGoalDist3D = sqrtf(dx * dx + dy * dy + dz * dz);
		}
	}
}

static void CountFindPathWave(FindPathFullCtx& c)
{
	// Which arm this waved search counts against. The queue priority of the
	// request being served is the label; without one the search is counted as
	// unattributed rather than assigned to either side. Resolved once here and
	// reused for the outcome, so a search cannot start on one arm and finish
	// on another.
	// Three labels for one search, resolved once here and reused for the
	// outcome so a search cannot start on one arm and finish on another.
	//
	// waveOwner is the engine's own verdict and is the arm a rate should be
	// read off. waveGateIdx is the label the connectivity check had to use,
	// because the engine had not resolved the character yet when it answered;
	// the two differ exactly where that early label is wrong, which is what
	// makes the check's miss rate readable. waveTagIdx is the in-flight
	// counter, kept as a third opinion because it can drift on its own.
	c.waveOwner   = WAVE_UNK;
	c.waveGateIdx = WAVE_UNK;
	c.waveTagIdx  = 0;
	if (t_waveAuth)
	{
		c.waveOwner = (t_waveAuth == 2) ? WAVE_PLAYER : WAVE_NPC;
		t_waveAuth = 0;
	}
	if (c.pathPoolPlayerByReq >= 0)
		c.waveGateIdx = c.pathPoolPlayerByReq ? WAVE_PLAYER : WAVE_NPC;
	c.waveTagIdx = currentRequestIsPlayer ? 1 : 0;
	if (c.wavedThrough)
	{
		InterlockedIncrement(&pathfind::g_pathDiag.diagWaveStarted[c.waveOwner]);
		InterlockedIncrement(&pathfind::g_pathDiag.diagWaveStartedByGate[c.waveGateIdx]);
		InterlockedIncrement(&pathfind::g_pathDiag.diagWaveStartedByTag[c.waveTagIdx]);
		if (c.waveOwner != WAVE_UNK && c.waveGateIdx != WAVE_UNK && c.waveOwner != c.waveGateIdx)
			InterlockedIncrement(&pathfind::g_pathDiag.diagWaveLabelDisagree);
	}
}

static void ProbeFindPathInput(void* searchState)
{
	// One-time FindPathInput layout probe
	if (searchState && !InterlockedCompareExchange(&pathfind::g_pathDiag.probeFPIDumped, 1, 0))
	{
		uintptr_t ss = (uintptr_t)searchState;

		float* sp = (float*)(ss + 16);
		for (int i = 0; i < 4; ++i)
			pathfind::g_pathDiag.probeStartPos[i] = sp[i];

		float** goalPtrAddr = (float**)(ss + 32);
		float* goalPtr = *goalPtrAddr;
		if (goalPtr)
		{
			for (int i = 0; i < 4; ++i)
				pathfind::g_pathDiag.probeGoalPos[i] = goalPtr[i];
			InterlockedExchange(&pathfind::g_pathDiag.probeGoalPtrValid, 1);
		}
		else
		{
			for (int i = 0; i < 4; ++i)
				pathfind::g_pathDiag.probeGoalPos[i] = 0.0f;
			InterlockedExchange(&pathfind::g_pathDiag.probeGoalPtrValid, 0);
		}

		int offsets[14] = { 40, 44, 48, 52, 56, 60, 64, 68,
		                    72, 76, 128, 136, 156, 160 };
		for (int i = 0; i < 14; ++i)
			pathfind::g_pathDiag.probeFields[i] = *(int*)(ss + offsets[i]);

		InterlockedExchange(&pathfind::g_pathDiag.probeFPIDumped, 2);
	}
}

static void BoostFindPathBudget(void* searchState, FindPathFullCtx& c)
{
	// Boost A* budget for player characters to prevent SEARCH_STATE_FULL stalls.
	// Player requests tagged by hook_csFindPath via playerRequestsInFlight counter.
	if (currentRequestIsPlayer && searchState)
	{
		uintptr_t ss = (uintptr_t)searchState;
		*(int*)(ss + 156) = 131072 * 4;   // open set: 512KB (default 128KB)
		*(int*)(ss + 160) = 590336 * 4;   // search state: ~2.3MB (default 590KB)
		c.pathPoolBoosted = 1;
	}
}

static void CaptureFindPathProbe(void* searchState, FindPathFullCtx& c)
{
	// Multi-call probe: capture FindPathInput positions before calling orig
	c.probeSlot = -1;
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.pathProbeArmed, 0, 0) == 1 && searchState)
	{
		c.probeSlot = InterlockedIncrement(&pathfind::g_pathDiag.pathProbeWriteIdx) - 1;
		if (c.probeSlot < PATH_PROBE_SIZE)
		{
			uintptr_t ss = (uintptr_t)searchState;
			float* sp = (float*)(ss + 16);
			float** gpa = (float**)(ss + 32);
			float* gp = *gpa;

			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].startX = sp[0];
			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].startY = sp[1];
			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].startZ = sp[2];
			if (gp)
			{
				pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].destX = gp[0];
				pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].destY = gp[1];
				pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].destZ = gp[2];
			}
			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].hookType = 2;
			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].result   = 0;
			pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].faceKey  = *(unsigned int*)(ss + 48);
		}
	}
}

static void RecordFindPathSamples(const FindPathFullCtx& c)
{
	LONGLONG astarTicks = c.pathPoolQpcAfter.QuadPart - c.pathPoolQpcBefore.QuadPart;
	bool     astarIsCause3 = (c.status == 3 && c.cause == 3);

	PathSearchSample pathPoolSample;
	pathPoolSample.ticks      = astarTicks;
	pathPoolSample.iterations = c.iterCount;
	pathPoolSample.status     = (int)c.status;
	pathPoolSample.cause      = (int)c.cause;
	pathPoolSample.boosted    = c.pathPoolBoosted;
	pathPoolSample.playerByTag = currentRequestIsPlayer ? 1 : 0;
	pathPoolSample.playerByReq  = c.pathPoolPlayerByReq;
	pathPoolSample.callerClass  = (int)c.astarCallerClass;
	pathPoolSample.isCause3     = astarIsCause3 ? 1 : 0;
	PathPoolNoteSearch(&pathPoolSample);

	AstarCostSample astarSample;
	astarSample.ticks        = astarTicks;
	astarSample.iterations   = c.iterCount;
	astarSample.status       = (int)c.status;
	astarSample.cause        = (int)c.cause;
	astarSample.boosted      = c.pathPoolBoosted;
	astarSample.playerByReq  = c.pathPoolPlayerByReq;
	astarSample.returnAddr   = c.astarReturnAddr;
	astarSample.startFaceKey = c.astarStartFaceKey;
	astarSample.goalFaceKey  = c.astarGoalFaceKey;
	astarSample.goalDist3D   = c.astarGoalDist3D;
	AstarCostNote(&astarSample, c.astarCallerClass);
}

static void RecordFindPathOutcome(void* searchState, const FindPathFullCtx& c)
{
	if (c.probeSlot >= 0 && c.probeSlot < PATH_PROBE_SIZE)
		pathfind::g_pathDiag.pathProbeBuf[c.probeSlot].result = (int)c.status;

	// The outcome of a search the cluster graph would have refused. Same
	// two bytes the buckets below read, no extra dereference. "Other" is
	// the named catch-all for every status but 1, 2, 3 and 5 -- truncated
	// included, since the full buckets below show whether any occurred.
	if (c.wavedThrough)
	{
		if (c.status == 1)      InterlockedIncrement(&pathfind::g_pathDiag.diagWaveSuccess[c.waveOwner]);
		else if (c.status == 2) InterlockedIncrement(&pathfind::g_pathDiag.diagWaveUnreach[c.waveOwner]);
		else if (c.status == 3) InterlockedIncrement(&pathfind::g_pathDiag.diagWaveTerm[c.waveOwner]);
		else if (c.status == 5) InterlockedIncrement(&pathfind::g_pathDiag.diagWaveInvalid[c.waveOwner]);
		else                  InterlockedIncrement(&pathfind::g_pathDiag.diagWaveOther[c.waveOwner]);

		if (c.status == 1)
		{
			InterlockedIncrement(&pathfind::g_pathDiag.diagWaveSuccessByGate[c.waveGateIdx]);
			InterlockedIncrement(&pathfind::g_pathDiag.diagWaveSuccessByTag[c.waveTagIdx]);
		}
	}

	if (c.status == 1)
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarSuccess);
	else if (c.status == 2)
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarUnreachable);
	else if (c.status == 3)
	{
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarTerminated);

		if (c.cause == 1)
			InterlockedIncrement(&pathfind::g_pathDiag.diagTermIterLimit);
		else if (c.cause == 2)
			InterlockedIncrement(&pathfind::g_pathDiag.diagTermOpenSetFull);
		else if (c.cause == 3)
		{
			InterlockedIncrement(&pathfind::g_pathDiag.diagTermStatesFull);
			// playerCap= on PathRate:: the node cap hit on a request the
			// queue already carried at player priority.
			// pathPoolPlayerByReq was resolved above, before
			// orig_findPathFull, from the same req+0x2C the queue itself
			// reads.
			if (c.pathPoolPlayerByReq == 1)
				InterlockedIncrement(&pathfind::g_pathDiag.diagPlayerCap);
		}
		else
			InterlockedIncrement(&pathfind::g_pathDiag.diagTermOtherCause);

		InterlockedExchange(&pathfind::g_pathDiag.diagLastTermIter, (long)c.iterCount);
	}
	else if (c.status == 4)
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarTruncated);
	else if (c.status == 5)
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarInvalid);
	else
		InterlockedIncrement(&pathfind::g_pathDiag.diagAstarOther);

	// Update max iterations high-water mark (lock-free CAS loop)
	long prev;
	do {
		prev = InterlockedCompareExchange(&pathfind::g_pathDiag.diagMaxIterUsed, 0, 0);
		if ((long)c.iterCount <= prev)
			break;
	} while (InterlockedCompareExchange(&pathfind::g_pathDiag.diagMaxIterUsed, (long)c.iterCount, prev) != prev);

	// Record last failure detail for player exposure reporting
	if (c.status != 1 && c.status != 2)
	{
		float* goalPtr = searchState ? *(float**)((uintptr_t)searchState + 32) : NULL;
		if (goalPtr)
		{
			pathfind::g_pathDiag.lastAstarFail.goalX = goalPtr[0];
			pathfind::g_pathDiag.lastAstarFail.goalY = goalPtr[1];
			pathfind::g_pathDiag.lastAstarFail.goalZ = goalPtr[2];
		}
		InterlockedExchange(&pathfind::g_pathDiag.lastAstarFail.status, (long)c.status);
		InterlockedExchange(&pathfind::g_pathDiag.lastAstarFail.cause, (long)c.cause);
		InterlockedExchange(&pathfind::g_pathDiag.lastAstarFail.iterCount, (long)c.iterCount);
		InterlockedIncrement(&pathfind::g_pathDiag.lastAstarFail.sequence);
	}

	// Per-player failure: record to ring buffer for main-thread logging
	if (c.status != 1 && currentRequestIsPlayer)
	{
		float* goalPtr = searchState ? *(float**)((uintptr_t)searchState + 32) : NULL;
		long idx = InterlockedIncrement(&pathfind::g_pathDiag.playerFailWriteIdx) - 1;
		int slot = (int)(idx % PLAYER_FAIL_RING);
		pathfind::g_pathDiag.playerFailRing[slot].goalX = goalPtr ? goalPtr[0] : 0;
		pathfind::g_pathDiag.playerFailRing[slot].goalZ = goalPtr ? goalPtr[2] : 0;
		InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].status, (long)c.status);
		InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].cause, (long)c.cause);
		InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].iterCount, (long)c.iterCount);
		InterlockedExchange(&pathfind::g_pathDiag.playerFailRing[slot].valid, 1);
	}
}

} // namespace pathfind_hooks_detail
using namespace pathfind_hooks_detail;

void hook_findPathFull(void* streamingCollection, void* searchState, void* findPathOutput)
{
	InterlockedIncrement(&pathfind::g_pathDiag.diagAstarAttempts);

	// This function's own return address is one of six known call sites
	// (astar_cost_policy.h) -- captured once, up front, since it never
	// changes across the rest of this call.
	void* astarReturnAddr = _ReturnAddress();

	FindPathFullCtx c;
	c.astarReturnAddr = astarReturnAddr;
	TakeFindPathWave(c);
	ResolveFindPathRequest(c);
	ReadFindPathInput(searchState, c);
	CountFindPathWave(c);
	c.pathPoolBoosted = 0;
	c.pathPoolQpcBefore.QuadPart = 0;

	ProbeFindPathInput(searchState);
	BoostFindPathBudget(searchState, c);
	CaptureFindPathProbe(searchState, c);

	// The original A*, once, or with its hierarchical arm (astar_hier.h)
	QueryPerformanceCounter(&c.pathPoolQpcBefore);
	AstarHierSearch(streamingCollection, searchState, findPathOutput, astarReturnAddr, c.pathPoolPlayerByReq);

	c.status = *(unsigned char*)((uintptr_t)findPathOutput + 60);
	c.cause  = *(unsigned char*)((uintptr_t)findPathOutput + 61);
	c.iterCount        = *(int*)((uintptr_t)findPathOutput + 48);

	// Take the "after" QPC and hand the sample to the pool on every
	// thread, every call.
	QueryPerformanceCounter(&c.pathPoolQpcAfter);
	RecordFindPathSamples(c);
	RecordFindPathOutcome(searchState, c);
}


// =========================================================================
// Hook 4: HavokCharacter::requestPath (main thread for a player's own order;
// the AI back thread for a mid-walk re-request or an AI-issued order)
// =========================================================================
// RVA 0x145CB0. RCX=HavokCharacter*, RDX=dest(float*), R8=priority(int)
// priority==2: player character, priority==0: NPC


void hook_requestPath(void* havokChar, float* destination, int priority)
{
	// Player vs NPC tracking
	if (priority >= 2)
		InterlockedIncrement(&pathfind::g_pathDiag.diagPlayerRequests);
	else
		InterlockedIncrement(&pathfind::g_pathDiag.diagNPCRequests);


	// The match is checked -- and counted by PlayerRepathTierIsPlayerOwned --
	// whether or not playerRepathTierEnabled arms it, so observe mode still
	// reports what it would do. Only checked for priority < 2; the order path
	// (priority >= 2) never needs it.
	bool matchedPlayerSet = (priority < 2) && PlayerRepathTierIsPlayerOwned((uintptr_t)havokChar);
	PlayerRepathTierDecision decision = PlayerRepathTierDecide(priority, matchedPlayerSet, pathfind::g_pathfindCfg.playerRepathTierEnabled);
	squadBoostTier = decision.tier;
	squadBoostFromRepathTier = (decision.source == PRT_SOURCE_STATE6 && decision.tier > 0);

	game::g_hookOrig.orig_requestPath(havokChar, destination, priority);

	squadBoostTier = 0;

}


// =========================================================================
// Hook 5: PathRequestQueue::submit (same thread as its only caller,
// requestPath -- main thread or the AI back thread)
// =========================================================================
// RVA 0x3AAEF0. Calls orig, then overwrites req+44 with boosted priority.


void hook_pathReqSubmit(void* sectionMgr, void* requestObj, bool highPriority)
{
	game::g_hookOrig.orig_pathReqSubmit(sectionMgr, requestObj, highPriority);

	if (squadBoostTier > 0)
	{
		int gamePri = *(int*)((uintptr_t)requestObj + 44);
		if (squadBoostTier > gamePri)
		{
			*(int*)((uintptr_t)requestObj + 44) = squadBoostTier;
			if (squadBoostFromRepathTier)
			{
				InterlockedIncrement(&p12DiagSubmitBoostsState6);
				PlayerRepathTierNoteTiered();
				// Not counted into playerRequestsInFlight: that counter feeds
				// the 4x A* search-budget boost in hook_findPathFull, a
				// separate lever this change does not touch. Only the queue
				// tier (req+44) changes here.
			}
			else
			{
				InterlockedIncrement(&p12DiagSubmitBoostsOrder);
				InterlockedIncrement(&pathfind::g_pathDiag.playerRequestsInFlight);
			}
		}
	}


	squadBoostTier = 0;
	squadBoostFromRepathTier = false;
}


// =========================================================================
// Hook 6: ContentStream::findPathFallback (contentStream bg thread)
// =========================================================================
// RVA 0x3AABF0. Called when csFindPath fails.


// Answers the reachability question directly: this fires whenever the
// primary A* search failed but a nearby point still resolved (NavMesh::update's
// only caller of this hook's site). Counts the hook running at all.
volatile long fallbackInvocations = 0;

char hook_csFindPathFallback(void* manager, unsigned int startFaceKey, void* startPos,
                              unsigned int destFaceKey, void* destPos, float radius,
                              float param6, char param7, void* resultBuf)
{
	InterlockedIncrement(&fallbackInvocations);

	// param7 is the engine's own answer to "is this request an NPC's", taken
	// from the owning character a few instructions before this call. The
	// search that consumes it runs inside the original below, on this thread,
	// so the stamp is set before and cleared after.
	t_waveAuth = param7 ? 1 : 2;


	char result = game::g_hookOrig.orig_csFindPathFallback(manager, startFaceKey, startPos,
	                                       destFaceKey, destPos, radius,
	                                       param6, param7, resultBuf);
	t_waveAuth = 0;
	return result;
}


// =========================================================================
// Extraction guard + addInstance timestamp
// =========================================================================

// hkaiStreamingCollection::addInstance (navmesh bg thread). Counts the
// insertions the section table takes and hands the registered instance to the
// lifecycle rows, which need the slot index the original writes. No blocking,
// no locks.
void hook_addInstance(void* collection, __int64 sectionData,
                      __int64 param3, __int64 param4, int param5)
{
	game::g_hookOrig.orig_addInstance(collection, sectionData, param3, param4, param5);
	InterlockedIncrement(&addInstanceHookCalls);
	NavMeshLifeOnAdd(collection, sectionData);
	StitchSourceOnAdd(collection, sectionData, param4);
}

// Havok::contentStreamCallee_0x8869 (contentStream bg thread): the loop that
// walks the search output and fills the result buffer. It dereferences the
// section an edge names without checking it, so an instance removed while the
// loop runs faults. On a fault the edge count is restored to what it was
// before the call, which logically erases the partial writes past it: the game
// reads "no new edges", treats it as no path and the character re-paths on the
// next tick instead of following a half-written result.
//
// Nothing in this function may need unwinding (a __try function takes no C++
// object with a destructor): POD locals, Win32 atomics and a literal log only.
static volatile long s_extractionLogged = 0;

unsigned __int64 hook_contentStreamCallee0x8869(void* manager,
                                                 unsigned int faceKey,
                                                 void* searchOutput,
                                                 unsigned int* resultBuf)
{
	unsigned int origCount = resultBuf ? (*(unsigned int*)KLIB_MEMBER(5, resultBuf, ResultPathArray_m_size, 8)) : 0;
	unsigned __int64 result = origCount;
	// A fault in the extraction chain is what this guard exists for, and
	// the rescue counter records it. Keep it out of the crash recorder.
	GuardEnter();
	__try {
		result = game::g_hookOrig.orig_contentStreamCallee0x8869(manager, faceKey, searchOutput, resultBuf);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement(&extractionCrashRescue);
		if (resultBuf) (*(unsigned int*)KLIB_MEMBER(5, resultBuf, ResultPathArray_m_size, 8)) = origCount;
		result = origCount;
		// A rescue is never silent. Bg thread, and no C++ object may live in a
		// function with __try: a literal through the deferrable logger only,
		// once per session (the counter above carries the rest).
		if (InterlockedExchange(&s_extractionLogged, 1) == 0)
			LogMsgDeferrable("PathGuard: fault inside the path-result extraction loop, "
			                 "result rolled back to no new edges");
	}
	GuardLeave();
	return result;
}


// "PathGuard:" line, main thread. Prints while anything happened and at most
// once per interval, independently of the priority-tier reporter.
void LogPathGuardStats(double now)
{
	static double lastLog = 0.0;
	static long   lastTotal = -1;

	long ecr    = InterlockedCompareExchange(&extractionCrashRescue, 0, 0);
	long aiHook = InterlockedCompareExchange(&addInstanceHookCalls, 0, 0);
	long evicts = InterlockedCompareExchange(&watchedEvictions, 0, 0);
	long total  = ecr + aiHook + evicts;

	if (total == 0 || total == lastTotal)
		return;
	if (now - lastLog < 30.0)
		return;
	lastLog   = now;
	lastTotal = total;

	std::ostringstream ss;
	ss << "PathGuard: extractAV=" << ecr
	   << " addInst=" << aiHook
	   << " evict=" << evicts;
	LogMsg(ss.str());
}

