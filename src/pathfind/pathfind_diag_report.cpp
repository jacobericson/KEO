// pathfind_diag_report.cpp - Diagnostic counter and failure reporting.
// Main thread. Snapshots are non-destructive; logging takes logCS inside LogMsg.

#include "pathfind/pathfind_diag.h"

namespace pathfind_diag_detail
{

// =========================================================================
// Reporter timing + per-window snapshots (main thread only)
// =========================================================================

static double lastPathDiagLogTime = 0.0;

static long prevPrimaryAttempts = 0;
static long prevPrimarySuccess  = 0;
static long prevPrimaryFail     = 0;
static long prevAstarAttempts   = 0;
static long prevAstarSuccess    = 0;
static long prevAstarUnreach    = 0;
static long prevAstarTerminated = 0;
static long prevAstarInvalid    = 0;
static long prevConnAttempts    = 0;
static long prevConnFail        = 0;
static long prevPlayerRequests  = 0;
static long prevNPCRequests     = 0;
static long prevPlayerCap       = 0;
static long prevFailSequence    = 0;

struct PathfindDiagReportCtx
{
	long pAttempts;
	long pSuccess;
	long pFail;
	long cAttempts;
	long cFail;
	long aAttempts;
	long aSuccess;
	long aUnreach;
	long aTermed;
	long aTruncated;
	long aInvalid;
	long aOther;
	long tIter;
	long tOpen;
	long tState;
	long tOther;
	long wStartedArm[3];
	long wSuccessArm[3];
	long wUnreachArm[3];
	long wTermArm[3];
	long wInvalidArm[3];
	long wOtherArm[3];
	long wStarted;
	long wSuccess;
	long wUnreach;
	long wTerm;
	long wInvalid;
	long wOther;
	long wGateArm[3];
	long wGateOkArm[3];
	long cRejArm[3];
	long wStale;
	long wTagNpc;
	long wTagPlayer;
	long wTagNpcOk;
	long wTagPlrOk;
	long wDisagree;
	long maxIter;
	long lastTIter;
};

void ReadPathfindDiagCounters(PathfindDiagReportCtx& c)
{
	// Snapshot all counters (non-destructive read)
	c.pAttempts = InterlockedCompareExchange(&pathfind::g_pathDiag.diagPrimaryAttempts, 0, 0);
	c.pSuccess  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagPrimarySuccess, 0, 0);
	c.pFail     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagPrimaryFail, 0, 0);

	c.cAttempts = InterlockedCompareExchange(&pathfind::g_pathDiag.diagConnAttempts, 0, 0);
	c.cFail     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagConnFail, 0, 0);

	c.aAttempts  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarAttempts, 0, 0);
	c.aSuccess   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarSuccess, 0, 0);
	c.aUnreach   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarUnreachable, 0, 0);
	c.aTermed    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarTerminated, 0, 0);
	c.aTruncated = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarTruncated, 0, 0);
	c.aInvalid   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarInvalid, 0, 0);
	c.aOther     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagAstarOther, 0, 0);

	c.tIter     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagTermIterLimit, 0, 0);
	c.tOpen     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagTermOpenSetFull, 0, 0);
	c.tState    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagTermStatesFull, 0, 0);
	c.tOther    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagTermOtherCause, 0, 0);

	c.wStarted = 0, c.wSuccess = 0, c.wUnreach = 0, c.wTerm = 0, c.wInvalid = 0, c.wOther = 0;
	for (int arm = 0; arm < 3; ++arm)
	{
		c.wStartedArm[arm] = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveStarted[arm], 0, 0);
		c.wSuccessArm[arm] = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveSuccess[arm], 0, 0);
		c.wUnreachArm[arm] = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveUnreach[arm], 0, 0);
		c.wTermArm[arm]    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveTerm[arm], 0, 0);
		c.wInvalidArm[arm] = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveInvalid[arm], 0, 0);
		c.wOtherArm[arm]   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveOther[arm], 0, 0);
		c.wStarted += c.wStartedArm[arm]; c.wSuccess += c.wSuccessArm[arm];
		c.wUnreach += c.wUnreachArm[arm]; c.wTerm    += c.wTermArm[arm];
		c.wInvalid += c.wInvalidArm[arm]; c.wOther   += c.wOtherArm[arm];
	}
	for (int garm = 0; garm < 3; ++garm)
	{
		c.wGateArm[garm]   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveStartedByGate[garm], 0, 0);
		c.wGateOkArm[garm] = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveSuccessByGate[garm], 0, 0);
		c.cRejArm[garm]    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagConnRejectByGate[garm], 0, 0);
	}
	c.wStale     = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveStale, 0, 0);
	c.wTagNpc    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveStartedByTag[0], 0, 0);
	c.wTagPlayer = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveStartedByTag[1], 0, 0);
	c.wTagNpcOk  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveSuccessByTag[0], 0, 0);
	c.wTagPlrOk  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveSuccessByTag[1], 0, 0);
	c.wDisagree  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagWaveLabelDisagree, 0, 0);

	c.maxIter   = InterlockedCompareExchange(&pathfind::g_pathDiag.diagMaxIterUsed, 0, 0);
	c.lastTIter = InterlockedCompareExchange(&pathfind::g_pathDiag.diagLastTermIter, 0, 0);
}

void AppendPathfindDiagBase(std::ostringstream& ss, const PathfindDiagReportCtx& c)
{
	// conn= carries the mode the answers were given under and the rejection
	// count in every mode, so an absent count means no session rather than no
	// rejections. Under the plain bypass the count stays 0 because no answer is
	// computed; measure computes it without obeying it, which is what makes
	// fail= and wave= readable from a single run.
	ss << "PathDiag:"
	   << " primary=" << c.pAttempts << "/" << c.pSuccess << "/" << c.pFail
	   << " conn=" << c.cAttempts
	   << (clusterGraphBypassMode == CGB_BYPASS ? "/bypass"
	       : (clusterGraphBypassMode == CGB_MEASURE ? "/measure"
	       : (clusterGraphBypassMode == CGB_PLAYER ? "/player" : "/orig")))
	   << "/fail=" << c.cFail
	   << " astar=" << c.aAttempts << "/" << c.aSuccess
	   << "/" << c.aUnreach << "unreach"
	   << "/" << c.aTermed << "term"
	   << "/" << c.aInvalid << "inv";

	if (c.aTruncated > 0)
		ss << "/" << c.aTruncated << "trunc";
	if (c.aOther > 0)
		ss << "/" << c.aOther << "oth";

	if (c.aTermed > 0)
		ss << " term=" << c.tIter << "iter/" << c.tOpen << "open/" << c.tState << "state";
	if (c.tOther > 0)
		ss << "/" << c.tOther << "oth";
}

void AppendPathfindDiagWave(std::ostringstream& ss, const PathfindDiagReportCtx& c)
{
	// What the bypass waves through, and how those searches end. Printed in
	// every mode so the token is never missing; outside measure mode no answer
	// is computed, and "n/a" says that instead of a zero that would read as
	// "the graph rejects nothing".
	//
	// The pooled totals answer "how much does the bypass wave through"; the
	// per-arm rows answer "for whom", which is the only form in which a rescue
	// rate means anything -- a rate over every request hides a high player rate
	// inside a large NPC denominator. unk is printed rather than folded away,
	// because a waved search whose request could not be identified is a fact
	// about the measurement, not a zero on either side.
	ss << " wave=";
	if (clusterGraphBypassMode == CGB_MEASURE || clusterGraphBypassMode == CGB_PLAYER)
	{
		ss << c.wStarted
		   << "/" << c.wSuccess << "succ"
		   << "/" << c.wUnreach << "unreach"
		   << "/" << c.wTerm << "term"
		   << "/" << c.wInvalid << "inv"
		   << "/" << c.wOther << "oth";

		// Three labelings of one population, each named after the label that
		// produced it, so two runs are never compared across a change of
		// label. waveAuth is the engine's verdict and the arm a rescue rate
		// is read off; waveGate is what the connectivity check had to decide
		// on; waveTag is the in-flight counter.
		static const char* kWaveArm[3] = { "npc", "player", "unk" };
		ss << " waveAuth=";
		for (int arm = 0; arm < 3; ++arm)
			ss << (arm ? " " : "") << kWaveArm[arm] << ":" << c.wStartedArm[arm]
			   << "/" << c.wSuccessArm[arm] << "s"
			   << "/" << c.wUnreachArm[arm] << "u"
			   << "/" << c.wTermArm[arm] << "t"
			   << "/" << c.wInvalidArm[arm] << "i"
			   << "/" << c.wOtherArm[arm] << "o";

		ss << " waveGate=";
		for (int arm = 0; arm < 3; ++arm)
			ss << (arm ? " " : "") << kWaveArm[arm] << ":" << c.wGateArm[arm]
			   << "/" << c.wGateOkArm[arm] << "s";

		ss << " waveTag=npc:" << c.wTagNpc << "/" << c.wTagNpcOk << "s"
		   << " player:" << c.wTagPlayer << "/" << c.wTagPlrOk << "s"
		   << " waveDisagree=" << c.wDisagree
		   << " waveStale=" << c.wStale;

		// The rejections themselves, by the label the check decided on. Under
		// the player-only mode the npc and unk arms are the ones obeyed, and
		// no wave bucket can show them.
		ss << " gateRej=";
		for (int arm = 0; arm < 3; ++arm)
			ss << (arm ? " " : "") << kWaveArm[arm] << ":" << c.cRejArm[arm];
	}
	else
		ss << "n/a";
}

void AppendPathfindDiagIterations(std::ostringstream& ss, const PathfindDiagReportCtx& c)
{
	ss << " maxIter=" << c.maxIter;
	if (c.lastTIter > 0)
		ss << " lastTermIter=" << c.lastTIter;
}

void ReportPathfindWindow(const PathfindDiagReportCtx& c)
{
	// Per-window throughput (delta since last report)
	{
		long dPrimary  = c.pAttempts - prevPrimaryAttempts;
		long dPSuccess = c.pSuccess  - prevPrimarySuccess;
		long dPFail    = c.pFail     - prevPrimaryFail;
		long dAstar    = c.aAttempts - prevAstarAttempts;
		long dASuccess = c.aSuccess  - prevAstarSuccess;
		long dAUnreach = c.aUnreach  - prevAstarUnreach;
		long dATermed  = c.aTermed   - prevAstarTerminated;
		long dAInvalid = c.aInvalid  - prevAstarInvalid;
		long dConn     = c.cAttempts - prevConnAttempts;
		long dConnFail = c.cFail     - prevConnFail;

		long playerReqs = InterlockedCompareExchange(&pathfind::g_pathDiag.diagPlayerRequests, 0, 0);
		long npcReqs    = InterlockedCompareExchange(&pathfind::g_pathDiag.diagNPCRequests, 0, 0);
		long dPlayer = playerReqs - prevPlayerRequests;
		long dNPC    = npcReqs    - prevNPCRequests;

		long playerCap  = InterlockedCompareExchange(&pathfind::g_pathDiag.diagPlayerCap, 0, 0);
		long dPlayerCap = playerCap - prevPlayerCap;

		prevPrimaryAttempts = c.pAttempts;
		prevPrimarySuccess  = c.pSuccess;
		prevPrimaryFail     = c.pFail;
		prevAstarAttempts   = c.aAttempts;
		prevAstarSuccess    = c.aSuccess;
		prevAstarUnreach    = c.aUnreach;
		prevAstarTerminated = c.aTermed;
		prevAstarInvalid    = c.aInvalid;
		prevConnAttempts    = c.cAttempts;
		prevConnFail        = c.cFail;
		prevPlayerRequests  = playerReqs;
		prevNPCRequests     = npcReqs;
		prevPlayerCap       = playerCap;

#ifdef ZONEOPT_DEBUG
		double interval = 10.0;
#else
		double interval = 30.0;
#endif

		// dConn keeps the window alive when the connectivity check is the only
		// thing still moving, which is what an arm that rejects most pairs
		// before the search looks like.
		if (dPrimary > 0 || dAstar > 0 || dConn > 0)
		{
			long dAFail = dAstar - dASuccess;
			int successPct = (dAstar > 0) ? (int)(100 * dASuccess / dAstar) : 0;

			std::ostringstream ts;
			ts << std::fixed << std::setprecision(1);
			int playerPct = (dPlayer + dNPC > 0) ? (int)(100 * dPlayer / (dPlayer + dNPC)) : 0;

			ts << "PathRate: "
			   << dPrimary << " req/" << (int)interval << "s"
			   << " (" << (dPrimary / interval) << "/s)"
			   << " player=" << dPlayer << "(" << playerPct << "%)"
			   << " npc=" << dNPC
			   << " direct=" << dPSuccess
			   << " fallback=" << dPFail
			   << " conn=" << dConn << "/rej=" << dConnFail
			   << " A*=" << dAstar
			   << " (" << (dAstar / interval) << "/s)"
			   << " ok=" << dASuccess << "(" << successPct << "%)"
			   << " fail=" << dAFail
			   // Node-cap terminations on an already player-tiered request
			   // (req+44 >= 20). Printed every window, 0 included, so a build
			   // that stops moving this is visible as a flat 0 rather than a
			   // missing token.
			   << " playerCap=" << dPlayerCap;
			if (dAFail > 0)
			{
				ts << "[unreach=" << dAUnreach;
				if (dATermed > 0)
					ts << " term=" << dATermed;
				if (dAInvalid > 0)
					ts << " inv=" << dAInvalid;
				ts << "]";
			}

			// Player failure exposure estimate
			if (dAFail > 0 && dPlayer > 0 && (dPlayer + dNPC) > 0)
			{
				int estPlayerFails = (int)((double)dAFail * dPlayer / (dPlayer + dNPC));
				ts << " playerExposure=~" << estPlayerFails;
			}

			LogMsg(ts.str());
		}

		// Last failure detail (when new failures since last report)
		long failSeq = InterlockedCompareExchange(&pathfind::g_pathDiag.lastAstarFail.sequence, 0, 0);
		if (failSeq > prevFailSequence)
		{
			float gx = pathfind::g_pathDiag.lastAstarFail.goalX * 10.0f;
			float gz = pathfind::g_pathDiag.lastAstarFail.goalZ * 10.0f;
			long st = InterlockedCompareExchange(&pathfind::g_pathDiag.lastAstarFail.status, 0, 0);
			long ca = InterlockedCompareExchange(&pathfind::g_pathDiag.lastAstarFail.cause, 0, 0);
			long it = InterlockedCompareExchange(&pathfind::g_pathDiag.lastAstarFail.iterCount, 0, 0);

			std::ostringstream fs;
			fs << std::fixed << std::setprecision(0);
			fs << "LastFail: status=" << st << " cause=" << ca
			   << " iter=" << it
			   << " worldGoal=(" << gx << "," << gz << ")";
			LogMsg(fs.str());
			prevFailSequence = failSeq;
		}
	}
}

void DrainPlayerPathFailures()
{
	// Drain player failure ring buffer
	for (int i = 0; i < PLAYER_FAIL_RING; ++i)
	{
		if (InterlockedCompareExchange(&pathfind::g_pathDiag.playerFailRing[i].valid, 0, 1) == 1)
		{
			float gx = pathfind::g_pathDiag.playerFailRing[i].goalX * 10.0f;
			float gz = pathfind::g_pathDiag.playerFailRing[i].goalZ * 10.0f;
			long st = pathfind::g_pathDiag.playerFailRing[i].status;
			long ca = pathfind::g_pathDiag.playerFailRing[i].cause;
			long it = pathfind::g_pathDiag.playerFailRing[i].iterCount;

			const char* reason = "unknown";
			if (st == 99) reason = "CLUSTER_GRAPH_REJECTED";
			else if (st == 2) reason = "UNREACHABLE";
			else if (st == 3 && ca == 1) reason = "ITER_LIMIT";
			else if (st == 3 && ca == 2) reason = "OPEN_SET_FULL";
			else if (st == 3 && ca == 3) reason = "SEARCH_STATE_FULL";
			else if (st == 3) reason = "TERMINATED";
			else if (st == 5) reason = "INVALID_START";

			std::ostringstream pf;
			pf << std::fixed << std::setprecision(0);
			pf << "PLAYER PATH FAIL: " << reason
			   << " status=" << st << " cause=" << ca
			   << " iter=" << it
			   << " worldGoal=(" << gx << "," << gz << ")";
			LogMsg(pf.str());
		}
	}
}

void ReportFindPathInputProbe()
{
	// One-time FindPathInput layout probe report
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.probeFPIDumped, 2, 2) == 2)
	{
		InterlockedExchange(&pathfind::g_pathDiag.probeFPIDumped, 3);

		std::ostringstream ps;
		ps << std::fixed << std::setprecision(2);
		ps << "FindPathInput probe:"
		   << " startPos=(" << pathfind::g_pathDiag.probeStartPos[0] << "," << pathfind::g_pathDiag.probeStartPos[1]
		   << "," << pathfind::g_pathDiag.probeStartPos[2] << "," << pathfind::g_pathDiag.probeStartPos[3] << ")";

		if (pathfind::g_pathDiag.probeGoalPtrValid)
			ps << " goalPos=(" << pathfind::g_pathDiag.probeGoalPos[0] << "," << pathfind::g_pathDiag.probeGoalPos[1]
			   << "," << pathfind::g_pathDiag.probeGoalPos[2] << "," << pathfind::g_pathDiag.probeGoalPos[3] << ")";
		else
			ps << " goalPtr=NULL";

		ps << " +40=" << pathfind::g_pathDiag.probeFields[0]
		   << " +44=" << pathfind::g_pathDiag.probeFields[1]
		   << " +48=" << pathfind::g_pathDiag.probeFields[2]
		   << " +52=" << pathfind::g_pathDiag.probeFields[3];

		ps << " +64=" << pathfind::g_pathDiag.probeFields[6]
		   << " +68=" << pathfind::g_pathDiag.probeFields[7]
		   << " +72=" << pathfind::g_pathDiag.probeFields[8]
		   << " +76=" << pathfind::g_pathDiag.probeFields[9];

		ps << " +128=" << pathfind::g_pathDiag.probeFields[10]
		   << " +136=" << pathfind::g_pathDiag.probeFields[11];

		ps << " +156=" << pathfind::g_pathDiag.probeFields[12]
		   << " +160=" << pathfind::g_pathDiag.probeFields[13];

		ps << " | +40f=" << std::setprecision(4) << *(float*)&pathfind::g_pathDiag.probeFields[0]
		   << " +76f=" << *(float*)&pathfind::g_pathDiag.probeFields[9];

		LogMsg(ps.str());
	}
}

} // namespace pathfind_diag_detail
using namespace pathfind_diag_detail;

// =========================================================================
// LogPathfindDiagStats (called from hook_updateCameraZone)
// =========================================================================

void LogPathfindDiagStats(double now)
{
	if (!pathfindDiagEnabled)
		return;

#ifdef ZONEOPT_DEBUG
	if (now - lastPathDiagLogTime < 10.0)
#else
	if (now - lastPathDiagLogTime < 30.0)
#endif
		return;
	lastPathDiagLogTime = now;

	PathfindDiagReportCtx c;
	ReadPathfindDiagCounters(c);
	if (c.pAttempts == 0 && c.aAttempts == 0 && c.cAttempts == 0)
		return;

	std::ostringstream ss;
	AppendPathfindDiagBase(ss, c);
	AppendPathfindDiagWave(ss, c);
	AppendPathfindDiagIterations(ss, c);

	// Connectivity traffic promotes the line to the log in both modes: the A/B
	// reads conn= and astar= together, and a line only one arm writes cannot
	// be compared against the other.
	if (c.aTermed > 0 || c.aUnreach > 0 || c.cAttempts > 0)
		LogMsg(ss.str());
	else
		LogDebug(ss.str());

	ReportPathfindWindow(c);
	DrainPlayerPathFailures();
	ReportFindPathInputProbe();
}
