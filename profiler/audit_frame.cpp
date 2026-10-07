// audit_frame.cpp - Frame state and world sampling.
// Main thread reads game memory and publishes frame records.
// Status lines use the output queue, which takes g_lineCS alone.

#include "audit_detail.h"
#include "audit_steady.h"
#include "audit_offmain.h"
#include <time.h>

namespace kenshiframeaudit_detail {
// Read once a second by OncePerSecond, copied into every frame (main thread).
static int s_factionsN = 0, s_unloadedPlatoons = 0, s_players = 0;

void OpenFrame(LONGLONG t, bool viaRenderOneFrame)
{
	memset(&g_cur, 0, sizeof(g_cur));
	g_cur.open  = true;
	g_cur.seq   = g_nextSeq++;
	g_cur.R0    = t;
	if (!viaRenderOneFrame)
		g_cur.flags |= F_BOUNDARY_T;

	float nan = Nan();
	g_cur.aiWake = g_cur.aiRun = g_cur.aiWindow = g_cur.aiZone = g_cur.aiContent = nan;
	g_cur.aiFactions = g_cur.aiVis = g_cur.aiOther = g_cur.aiResid = nan;
	g_cur.aiTU = g_cur.aiTU4 = g_cur.aiTUP = g_cur.aiTUmax = nan;
	g_cur.aiEnv = g_cur.aiForced = g_cur.aiL1Task = g_cur.aiL1Move = g_cur.aiL1Flush = g_cur.aiL1Anim = nan;
	g_cur.birdsWake = g_cur.birdsRun = g_cur.birdsWindow = g_cur.physWake = g_cur.physRun = nan;
	g_cur.physLock = g_cur.physPre = g_cur.physSim = g_cur.physPost = nan;
	g_cur.dtMs = g_cur.speed = nan;
	g_cur.relMs = g_cur.afPlatoonU = nan;

	// No scene call spans a frame boundary; an exception can leave one open.
	g_depth    = 0;
	g_rsoDepth = 0;
	g_npending = 0;

	g_sceneMgr = MainSceneManager();
}

// Render sums for the frame record; also flushes culls no render claimed.
void FinishRender(CurFrame& c, FrameRec& r, unsigned* flags)
{
	for (int i = 0; i < g_npending; ++i)
	{
		if (c.ncalls >= MAX_RCALLS)
		{
			*flags |= F_RCALLSFULL;
			break;
		}
		const PendingCull& p = g_pending[i];
		RCall& rc = c.calls[c.ncalls++];
		memset(&rc, 0, sizeof(rc));
		rc.cam     = p.camId;
		rc.vp      = p.vpId;
		rc.firstRq = p.firstRq;
		rc.lastRq  = p.lastRq;
		rc.flags   = (unsigned char)(RC_CULLONLY | (p.shadow ? RC_SHADOW : 0));
		rc.cullMs  = p.ms;
		rc.rsoMs = rc.setPassMs = rc.bindMs = rc.d3dMs = rc.syncMs = Nan();
	}
	g_npending = 0;

	r.ncalls = c.ncalls;
	if (c.ncalls > 0)
		memcpy(r.calls, c.calls, sizeof(RCall) * (size_t)c.ncalls);
	if (g_haveTag[ST_LIGHTS])
		r.m[M_R_LIGHTS] = TicksToMs(c.sub[SUBT_LIGHTS]);   // light renderer, CSM included

	if (!g_renderOn)
		return;
	double perMs = g_tscPerMs;
	if (g_syncHooked)
	{
		r.m[M_R_SYNC] = TicksToMs(c.syncTicks);
		r.c[C_SYNCS]  = c.syncs;
	}
	if (g_oldAnimHooked)
	{
		r.m[M_R_OLDANIM] = TicksToMs(c.oldAnimTicks);
		r.c[C_OLDANIMS]  = c.oldAnims;
	}
	if (g_bindHooked)
	{
		r.m[M_R_BIND] = perMs > 0.0 ? (float)((double)c.bindTsc / perMs) : Nan();
		r.c[C_BINDS]  = c.binds;
	}
	if (g_d3dHooked)
		r.m[M_R_D3D] = perMs > 0.0 ? (float)((double)c.d3dTsc / perMs) : Nan();
	float cull = 0.0f, scene = 0.0f, shadow = 0.0f, submit = 0.0f, rso = 0.0f, setPass = 0.0f;
	bool timed = true;
	for (int i = 0; i < c.ncalls; ++i)
	{
		const RCall& rc = c.calls[i];
		cull += rc.cullMs;
		if (rc.flags & RC_CULLONLY)
			continue;
		if (rc.flags & RC_SHADOW) shadow += rc.ms;
		else                      scene  += rc.ms;
		submit += rc.submitMs;
		if (IsNan(rc.rsoMs)) timed = false;
		else { rso += rc.rsoMs; setPass += rc.setPassMs; }
	}
	// A frame whose call table overflowed has partial sums: leave them out.
	if (!(*flags & F_RCALLSFULL))
	{
		r.m[M_R_CULL]    = cull;
		r.m[M_R_SCENE]   = scene;
		r.m[M_R_SHADOW]  = shadow;
		r.m[M_R_SUBMIT]  = submit;
		r.m[M_R_QUEUE]   = scene + shadow - submit;
		r.m[M_R_RSO]     = timed ? rso : Nan();
		r.m[M_R_SETPASS] = timed ? setPass : Nan();
		if (g_cmHooked && !IsNan(r.m[M_PASSES]))
			r.m[M_R_PASSOTHER] = r.m[M_PASSES] - cull - scene - shadow;
	}
	r.c[C_INSTDRAWS]   = c.instDraws;
	r.c[C_INSTANCES]   = c.instances;
	r.c[C_SETPASSES]   = c.setPasses;
	r.c[C_RSOS]        = c.rsos;
	r.c[C_DRAWS_OUT]   = c.drawsOut;
	r.c[C_DRAWS_NORSO] = c.drawsNoRso;
	r.c[C_SCENECALLS]  = c.sceneCalls;
	InterlockedIncrement(&g_drawFrames);
}

void CloseFrame(LONGLONG tNext)
{
	CurFrame& c = g_cur;
	FrameRec r;
	r.seq = c.seq;
	r.t   = SinceStart(c.R0);
	for (int m = 0; m < NUM_METRICS; ++m) r.m[m] = Nan();
	for (int k = 0; k < NUM_COUNTS; ++k)  r.c[k] = 0;
	r.ncalls = 0;
	memset(r.curGroups, 0, sizeof(r.curGroups));
	memset(&r.split, 0, sizeof(r.split));
	memset(&r.phys, 0, sizeof(r.phys));
	r.nphysq = 0;

	unsigned flags = c.flags;
	if (g_foreground) flags |= F_FG;
	if (InterlockedExchange(&g_transSeen, 0) || g_transOpen) flags |= F_TRANS;
	bool inGame = c.hM0 && c.hM1;
	if (inGame) flags |= F_INGAME;
	if (inGame && c.zoneState != 0) flags |= F_ZONEBUSY;

	float frame = TicksToMs(tNext - c.R0);
	r.m[M_FRAME] = frame;
	if (frame > g_cfg.stallMs) flags |= F_STALL;

	bool listeners = c.hT0 && c.hT1 && c.hQ0 && c.hQ1 && c.hE0 && c.hE1;
	if (!listeners || c.probeDepth != 0 || c.cmDepth != 0)
		flags |= F_BROKEN;

	if (listeners)
	{
		LONGLONG R1 = c.hR1 ? c.R1 : c.E1;
		r.m[M_PRE]     = TicksToMs(c.T0 - c.R0);
		r.m[M_START]   = TicksToMs(c.T1 - c.T0);
		r.m[M_SCENE]   = TicksToMs(c.sgTop + c.sgNested);
		r.m[M_PASSES]  = TicksToMs(c.cmTicks - c.sgNested);
		r.m[M_ROTHER]  = TicksToMs((c.Q0 - c.T1) - c.sgTop - c.cmTicks + (c.E0 - c.Q1) - c.swTicks);
		r.m[M_QUEUED]  = TicksToMs(c.Q1 - c.Q0);
		r.m[M_PRESENT] = TicksToMs(c.swTicks);
		if (inGame)
		{
			r.m[M_FEHEAD]   = TicksToMs(c.M0 - c.E0);
			r.m[M_MAINLOOP] = TicksToMs(c.M1 - c.M0);
			r.m[M_FETAIL]   = TicksToMs(c.E1 - c.M1);
		}
		else
		{
			r.m[M_FEHEAD]   = TicksToMs(c.E1 - c.E0);
			r.m[M_MAINLOOP] = 0.0f;
			r.m[M_FETAIL]   = 0.0f;
		}
		r.m[M_POST] = TicksToMs(R1 - c.E1);
		r.m[M_GAP]  = TicksToMs(tNext - R1);

		float sum = 0.0f;
		for (int m = M_PRE; m <= M_GAP; ++m)
		{
			sum += r.m[m];
			if (r.m[m] < -0.01f) flags |= F_BROKEN;   // stamps out of order
		}
		r.m[M_SUMERR] = frame - sum;
	}

	if (inGame)
	{
		LONGLONG sections = 0;
		for (int i = 0; i < ML_SECTIONS; ++i)
		{
			r.m[ML_FIRST + i] = TicksToMs(c.ml[i]);
			sections += c.ml[i];
		}
		r.m[M_ML_OTHER] = TicksToMs((c.M1 - c.M0) - sections);

		r.m[M_AI_WAKE] = c.aiWake;      r.m[M_AI_RUN] = c.aiRun;
		r.m[M_AI_WINDOW] = c.aiWindow;  r.m[M_AI_ZONE] = c.aiZone;
		r.m[M_AI_CONTENT] = c.aiContent; r.m[M_AI_FACTIONS] = c.aiFactions;
		r.m[M_AI_VIS] = c.aiVis;        r.m[M_AI_OTHER] = c.aiOther;
		r.m[M_AI_RESID] = c.aiResid;
		r.m[M_AI_TU] = c.aiTU;          r.m[M_AI_TU4] = c.aiTU4;
		r.m[M_AI_TUP] = c.aiTUP;        r.m[M_AI_TUMAX] = c.aiTUmax;
		r.m[M_AI_ENV] = c.aiEnv;        r.m[M_AI_FORCED] = c.aiForced;
		r.m[M_AI_L1TASK] = c.aiL1Task;  r.m[M_AI_L1MOVE] = c.aiL1Move;
		r.m[M_AI_L1FLUSH] = c.aiL1Flush; r.m[M_AI_L1ANIM] = c.aiL1Anim;
		r.m[M_BIRDS_WAKE] = c.birdsWake; r.m[M_BIRDS_RUN] = c.birdsRun;
		r.m[M_BIRDS_WINDOW] = c.birdsWindow;
		r.m[M_PHYS_WAKE] = c.physWake;  r.m[M_PHYS_RUN] = c.physRun;
		r.m[M_PHYS_LOCK] = c.physLock;  r.m[M_PHYS_PRE] = c.physPre;
		r.m[M_PHYS_SIM] = c.physSim;    r.m[M_PHYS_POST] = c.physPost;

		// Probes nested inside the player and zoneCam sections.
		if (g_haveTag[ST_MOUSESCAN]) r.m[M_SUB_MOUSESCAN] = TicksToMs(c.sub[SUBT_MOUSESCAN]);
		if (g_haveTag[ST_MOUSERAY])  r.m[M_SUB_MOUSERAY]  = TicksToMs(c.sub[SUBT_MOUSERAY]);
		if (g_haveTag[ST_CAMRAY])    r.m[M_SUB_CAMRAY]    = TicksToMs(c.sub[SUBT_CAMRAY]);
		if (g_haveTag[ST_MOUSERAY2]) r.m[M_SUB_MOUSERAY2] = TicksToMs(c.sub[SUBT_MOUSERAY2]);
		// The cursor ray per call (NaN on frames without one), and its qsort.
		if (g_haveTag[ST_MOUSERAY] && c.curCalls > 0)
		{
			float calls = (float)c.curCalls;
			float ray   = TicksToMs(c.sub[SUBT_MOUSERAY]) / calls;
			r.m[M_SUB_CURRAY] = ray;
			if (g_haveTag[ST_CURSORT])
			{
				float sort = TicksToMs(c.sub[SUBT_CURSORT]) / calls;
				r.m[M_SUB_CURSORT] = sort;
				r.m[M_SUB_CURCAST] = ray - sort;
			}
		}
		r.c[C_CURCALLS]  = c.curCalls;
		r.c[C_CURSTILL]  = c.curClass[CUR_STILL];
		r.c[C_CURSMALL]  = c.curClass[CUR_SMALL];
		r.c[C_CURLARGE]  = c.curClass[CUR_LARGE];
		r.c[C_CURMOVED]  = c.curMoved;
		r.c[C_CURBTN]    = c.curBtn;
		r.c[C_CUREDGE]   = c.curEdge;
		r.c[C_CURMOD]    = c.curMod;
		r.c[C_CURPHYS]   = c.curPhys;
		r.c[C_CURHITS]   = c.curHits;
		r.c[C_CURCHAR]   = c.curChar;
		r.c[C_RAY2CALLS] = c.ray2Calls;
		r.c[C_CURINPUT]  = (int)c.curInput;
		memcpy(r.curGroups, c.curGroups, sizeof(r.curGroups));
		if (g_haveTag[ST_ZC_CONTENT] && g_haveTag[ST_ZC_MISC] && g_haveTag[ST_ZC_ACT] &&
		    g_haveTag[ST_ZC_DEACT] && g_haveTag[ST_ZC_SECT] && g_haveTag[ST_ZC_MAINT])
		{
			// Vanilla updateCameraZone's own calls; the rest of the zoneCam
			// call-site time is whatever is hooked in front of it (the optimizer).
			LONGLONG van = c.sub[SUBT_ZCCONTENT] + c.sub[SUBT_ZCMISC] + c.sub[SUBT_ZCACT] + c.sub[SUBT_ZCSECT];
			r.m[M_SUB_ZCVAN]     = TicksToMs(van);
			r.m[M_SUB_ZCCONTENT] = TicksToMs(c.sub[SUBT_ZCCONTENT]);
			r.m[M_SUB_ZCSECT]    = TicksToMs(c.sub[SUBT_ZCSECT]);
			if (g_haveTag[ST_ZONECAM])
				r.m[M_SUB_ZCMOD] = TicksToMs(c.ml[M_ML_ZONECAM - ML_FIRST] - van);
		}

		r.m[M_ZONE_SM]   = c.zoneSMms;
		r.m[M_UNLOAD_MS] = TicksToMs(c.unloadTicks);
		r.m[M_DT]        = c.dtMs;
		r.m[M_SPEED]     = c.speed;
		if (c.camValid)
		{
			r.m[M_CAM_X] = c.cam[0];
			r.m[M_CAM_Y] = c.cam[1];
			r.m[M_CAM_Z] = c.cam[2];
		}
		r.m[M_CAM_ALT] = g_camAlt;

		r.c[C_CHARS]       = c.chars;
		r.c[C_DEAD]        = c.dead;
		r.c[C_FULLRATE]    = c.full;
		r.c[C_AIL1]        = c.aiL1;
		r.c[C_AIL4]        = c.aiL4;
		r.c[C_AILP]        = c.aiLP;
		r.c[C_VISCALLS]    = c.visCalls;
		r.c[C_SETB]        = c.setB;
		r.c[C_ZONESTATE]   = c.zoneState;
		r.c[C_ZLOADED]     = g_zLoaded;
		r.c[C_ZOUTB]       = g_zLoaded > c.setB ? g_zLoaded - c.setB : 0;
		r.c[C_UNLOADS]     = c.unloads;
		r.c[C_PLATOONS]    = g_platoons;
		r.c[C_FACTIONSN]   = s_factionsN;
		r.c[C_UNLPLATOONS] = s_unloadedPlatoons;
		r.c[C_PLAYERS]     = s_players;
		SteadyFrameTotals(c, r);
	}
	r.c[C_DRAWS]       = c.draws;
	r.c[C_SHADOWDRAWS] = c.shadowDraws;
	r.c[C_SQUADS]      = (int)InterlockedExchange(&g_squadsPending, 0);
	if (g_fxHooked)
		FxFrameTotals(r);   // every frame, so each delta covers one frame
	if (g_cfg.offMainDetail)
		OffMainFrameTotals(r);   // every frame, so the per-frame totals cover one frame
	if (c.fxCensused)
	{
		double perMs = g_tscPerMs;
		r.m[M_FX_CENSUS] = perMs > 0.0 ? (float)((double)c.fxCensusTsc / perMs) : Nan();
		r.c[C_FX]      = c.fx;
		r.c[C_FXSYS]   = c.fxSys;
		r.c[C_FXVIS]   = c.fxVis;
		r.c[C_FXSTOP]  = c.fxStop;
		r.c[C_FXPARTS] = c.fxParts;
		r.c[C_FXNEW]   = c.fxNew;
		r.c[C_FXDEL]   = c.fxDel;
	}
	FinishRender(c, r, &flags);
	if (c.curSplit)
	{
		// The shadow rays ran inside `player`: keep the frame out of every statistic.
		flags  |= F_CURSORSPLIT;
		r.split = c.split;
	}
	r.phys = c.phys;
	r.nphysq = c.nphysq > MAX_PHYS_QUERIES ? MAX_PHYS_QUERIES : c.nphysq;
	if (r.nphysq > 0)
		memcpy(r.physq, c.physq, sizeof(PhysQuerySample) * (size_t)r.nphysq);

	r.flags = flags;
	RingPush(r);
	c.open = false;
}

// Called at a frame boundary stamp (renderOneFrame entry, or frameStarted
// entry in fallback mode).
void FrameBoundary(LONGLONG t, bool viaRenderOneFrame)
{
	if (g_cur.open)
		CloseFrame(t);
	OpenFrame(t, viaRenderOneFrame);
}

// =========================================================================
// Worker data collection (main thread)
// =========================================================================

// At the start of processThreadMessages, the first call after mainLoop's AI
// join: the AI body for this frame's kick has finished.
void CollectAi()
{
	ThreadSlot& s = g_ai;
	LONG done = InterlockedCompareExchange(&s.doneSeq, 0, 0);
	if (done == 0 || done != (LONG)g_cur.seq || s.runSeq != done || s.collected == done)
	{
		if (!(g_cur.flags & F_AISYNC))
			g_cur.flags |= F_AIMISS;
		return;
	}
	_ReadBarrier();
	s.collected = done;

	g_cur.aiRun      = TicksToMs(s.bodyOut - s.bodyIn);
	g_cur.aiZone     = TicksToMs(s.zone);
	g_cur.aiContent  = TicksToMs(s.content);
	g_cur.aiFactions = TicksToMs(s.factions);
	g_cur.aiVis      = TicksToMs(s.vis);
	g_cur.aiOther    = g_cur.aiRun - g_cur.aiZone - g_cur.aiContent - g_cur.aiFactions - g_cur.aiVis;
	if (g_haveTag[ST_AITU] || g_haveTag[ST_AITU4] || g_haveTag[ST_AITUP])
	{
		g_cur.aiTU    = TicksToMs(s.tu);
		g_cur.aiTU4   = TicksToMs(s.tu4);
		g_cur.aiTUP   = TicksToMs(s.tup);
		g_cur.aiTUmax = TicksToMs(s.tuMax);
		g_cur.aiOther -= g_cur.aiTU + g_cur.aiTU4 + g_cur.aiTUP;
	}
	if (g_haveTag[ST_AIENV])
	{
		g_cur.aiEnv    = TicksToMs(s.env);
		g_cur.aiOther -= g_cur.aiEnv;
	}
	if (g_haveTag[ST_AIFORCED])
	{
		g_cur.aiForced = TicksToMs(s.forced);
		g_cur.aiOther -= g_cur.aiForced;
	}
	// Parts of list 1 (nested inside aiTU, not subtracted again).
	if (g_bodyHooked)          g_cur.aiL1Task  = TicksToMs(s.l1Task);
	if (g_moveHooked)          g_cur.aiL1Move  = TicksToMs(s.l1Move);
	if (g_haveTag[ST_AIFLUSH]) g_cur.aiL1Flush = TicksToMs(s.l1Flush);
	if (g_haveTag[ST_AIANIM])  g_cur.aiL1Anim  = TicksToMs(s.l1Anim);
	SteadyCollectAi(s);

	// Task classes of this run into the per-class counters.
	for (int i = 0; i < s.ntask && i < ThreadSlot::MAX_TASKS; ++i)
	{
		if (!s.taskVt[i])
		{
			g_taskNoneTicks += s.taskTicks[i];
			g_taskNoneCalls += s.taskCalls[i];
			continue;
		}
		int id = ClassIdOf(s.taskVt[i]);
		g_taskTicks[id] += s.taskTicks[i];
		g_taskCalls[id] += s.taskCalls[i];
	}
	InterlockedIncrement(&g_aiFrames);
	g_cur.aiL1       = s.l1;
	g_cur.aiL4       = s.l4;
	g_cur.aiLP       = s.lp;
	g_cur.visCalls   = s.visCalls;
	if (g_cur.aiKickT)
	{
		g_cur.aiWake = TicksToMs(s.bodyIn - g_cur.aiKickT);
		LONGLONG joinStart = g_cur.aiJoinT0 ? g_cur.aiJoinT0 : g_cur.M0;
		if (joinStart)
			g_cur.aiWindow = TicksToMs(joinStart - g_cur.aiKickT);
	}
	if (g_cur.aiJoinT0)
	{
		float wait  = TicksToMs(g_cur.aiJoinT1 - g_cur.aiJoinT0);
		float model = TicksToMs(s.bodyOut - g_cur.aiJoinT0);
		if (model < 0.0f) model = 0.0f;
		g_cur.aiResid = wait - model;
	}
}

// At the start of charsUpdate: the birds join (just before it) has completed
// the run kicked at the end of the previous mainLoop.
void CollectBirds()
{
	ThreadSlot& s = g_birds;
	LONG done = InterlockedCompareExchange(&s.doneSeq, 0, 0);
	// Only the kick from the immediately preceding frame: after a menu or a
	// save load the last run can be seconds old.
	if (done == 0 || done != s.kickSeq || s.runSeq != done || s.collected == done ||
	    done != (LONG)(g_cur.seq - 1))
		return;
	_ReadBarrier();
	s.collected = done;
	g_cur.birdsWake = TicksToMs(s.bodyIn - s.kickT);
	g_cur.birdsRun  = TicksToMs(s.bodyOut - s.bodyIn);
	if (g_cur.birdsJoinT0)
		g_cur.birdsWindow = TicksToMs(g_cur.birdsJoinT0 - s.kickT);
}

// At the start of processKillList: mainLoop only enters the physics block
// when the physics thread is idle, so its last run has finished.
void CollectPhys()
{
	ThreadSlot& s = g_phys;
	LONG done = InterlockedCompareExchange(&s.doneSeq, 0, 0);
	if (done == 0 || done != s.kickSeq || s.runSeq != done || s.collected == done)
		return;
	_ReadBarrier();
	s.collected = done;
	g_cur.physWake = TicksToMs(s.bodyIn - s.kickT);
	g_cur.physRun  = TicksToMs(s.bodyOut - s.bodyIn);
	if (g_haveTag[ST_PHYSLOCK] && g_haveTag[ST_PHYSPRE] && g_haveTag[ST_PHYSPOST])
	{
		// Between pre and post the body runs NxScene simulate / flushStream /
		// fetchResults(block) and one controller callback: all indirect calls.
		g_cur.physLock = TicksToMs(s.lock);
		g_cur.physPre  = TicksToMs(s.pre);
		g_cur.physPost = TicksToMs(s.post);
		g_cur.physSim  = g_cur.physRun - g_cur.physLock - g_cur.physPre - g_cur.physPost;
	}
	if (g_cfg.physxDetail)
	{
		PhysRunSample& p = g_cur.phys;
		memset(&p, 0, sizeof(p));
		p.valid   = true;
		p.runSeq  = s.runSeq;
		p.runMs   = TicksToMs(s.bodyOut - s.bodyIn);
		p.lockMs  = TicksToMs(s.lock);
		p.preMs   = TicksToMs(s.pre);
		p.hulls   = s.physHulls;
		for (int i = 0; i < PP_COUNT; ++i)
			p.phaseMs[i] = TicksToMs(s.physPhaseTicks[i]);
		for (int i = 0; i < PO_COUNT; ++i)
		{
			p.queued[i] = s.physQueued[i];
			p.calls[i]  = s.physCalls[i];
		}
		p.calls[PO_IMPULSE]      = -1; // five-argument virtual call: stack argument must remain untouched
		p.calls[PO_HULL_DESTROY] = -1; // two-byte virtual call site is intentionally unpatched
		for (int i = 0; i < hullpose::PC_COUNT; ++i)
			p.apClass[i] = s.hullClass[i];
	}
}

// =========================================================================
// World sampling (main thread, right after mainLoop)
// =========================================================================

// The enabled-compositor list (OptionsHolder::compositors), as name:0/1.
std::string CompositorsString(uintptr_t o)
{
	uintptr_t lk = KLIB_MEMBER(5, o, OptionsHolder_compositors, OPT_COMPOSITORS);
	unsigned count = *(const unsigned*)(KLIB_MEMBER(5, lk, CompositorLektor_count, LEKTOR_COUNT));
	uintptr_t data = *(const uintptr_t*)(KLIB_MEMBER(5, lk, CompositorLektor_stuff, LEKTOR_DATA));
	if (count == 0 || count > 64 || !PlausiblePtr(data))
		return "-";
	std::string s;
	for (unsigned i = 0; i < count; ++i)
	{
		uintptr_t entry = KlibCompositorEntry(data, i);
		char name[48];
		CopyOgreString((const void*)KLIB_MEMBER(5, entry, Compositor_first, 0), name, sizeof(name));
		bool on = *(const unsigned char*)(KLIB_MEMBER(5, entry, Compositor_second, COMPOSITOR_FLAG)) != 0;
		if (!s.empty()) s += ",";
		s += Fmt("%s:%d", name[0] ? name : "?", on ? 1 : 0);
	}
	return s;
}

std::string SettingsString()
{
	uintptr_t o = KlibAddress(g_base, RVA_OPTIONS);
	std::string s = Fmt("view=%.0f npcRange=%.0f objRange=%.0f foliage=%.0f grass=%.0f/%.2f terrain=%.2f pop=%.2f",
	                    *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_VIEW_DISTANCE, OPT_VIEW)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_NPCRange, OPT_NPC_RANGE)),
	                    *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_smallBuildingRange, OPT_OBJ_RANGE)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_foliageRange, OPT_FOLIAGE)),
	                    *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_grassRange, OPT_GRASS_RANGE)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_grassDensity, OPT_GRASS_DENS)),
	                    *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_terrainDetail, OPT_TERRAIN)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_populationMult, OPT_POPULATION)));
	s += Fmt(" shadows=%d shadowQ=%d shadowRange=%.0f water=%d reflDist=%.0f decal=%.0f feature=%.0f distTown=%.0f manyZones=%d fancy=%d names=%d charMT=%d",
	         *(const int*)(KLIB_MEMBER(5, o, OptionsHolder_shadowMode, OPT_SHADOWS)), *(const int*)(KLIB_MEMBER(5, o, OptionsHolder_shadowQuality, OPT_SHADOW_Q)),
	         *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_shadowRange, OPT_SHADOW_RANGE)), *(const int*)(KLIB_MEMBER(5, o, OptionsHolder_reflectionMode, OPT_WATER)),
	         *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_reflectionDistance, OPT_REFL_DIST)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_decalRange, OPT_DECAL_RANGE)),
	         *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_featureRange, OPT_FEATURE)), *(const float*)(KLIB_MEMBER(5, o, OptionsHolder_distantTownRange, OPT_DIST_TOWN)),
	         (int)*(const unsigned char*)(KLIB_MEMBER(5, o, OptionsHolder_manyActiveZones, OPT_MANY_ZONES)), (int)*(const unsigned char*)(KLIB_MEMBER(5, o, OptionsHolder_fancyShaders, OPT_FANCY)),
	         (int)*(const unsigned char*)(KLIB_MEMBER(5, o, OptionsHolder_showNames, OPT_NAMES)), (int)*(const unsigned char*)(KLIB_MEMBER(5, o, OptionsHolder_characterMultithreading, OPT_CHAR_MT)));
	s += " compositors=" + CompositorsString(o);
	return s;
}

std::string SystemString()
{
	char path[MAX_PATH];
	strcpy_s(path, "none");
	HMODULE opt = GetModuleHandleA("KEO.dll");
	if (opt)
		GetModuleFileNameA(opt, path, MAX_PATH);
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	int workers = -1;
	if (g_getWorkers)
	{
		void* sm = MainSceneManager();
		if (sm)
			workers = (int)g_getWorkers(sm);
	}
	return Fmt("optimizer=%s cpus=%u ogreWorkers=%d boundary=%s timerExp=%d",
	           path, (unsigned)si.dwNumberOfProcessors, workers,
	           g_boundaryR ? "renderOneFrame" : "frameStarted",
	           g_cfg.timerExperiment ? 1 : 0);
}

void OncePerSecond()
{
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg)
		GetWindowThreadProcessId(fg, &pid);
	g_foreground = (pid == GetCurrentProcessId());

	uintptr_t gw = KlibAddress(g_base, RVA_GAMEWORLD);

	int platoons = 0, unloaded = 0, factions = 0, players = 0;
	uintptr_t fm = *(uintptr_t*)(KLIB_MEMBER(5, gw, GameWorld_factionMgr, GW_FACTIONMGR));
	if (PlausiblePtr(fm))
	{
		unsigned count = *(const unsigned*)(KLIB_MEMBER(5, fm, FactionManager_participants_count, FM_COUNT));
		uintptr_t data = *(const uintptr_t*)(KLIB_MEMBER(5, fm, FactionManager_participants_stuff, FM_DATA));
		if (count <= 4096 && PlausiblePtr(data))
		{
			factions = (int)count;
			const uintptr_t* list = (const uintptr_t*)data;
			for (unsigned i = 0; i < count; ++i)
			{
				if (!PlausiblePtr(list[i]))
					continue;
				platoons += *(const int*)(KLIB_MEMBER(5, list[i], Faction_activePlatoons_count, FACTION_ACTIVE_PLATOONS));
				unloaded += *(const int*)(list[i] + FACTION_UNLOADED_PLATOONS);
			}
		}
	}
	g_platoons = platoons;
	s_factionsN = factions;
	s_unloadedPlatoons = unloaded;

	g_camAlt = Nan();
	uintptr_t player = *(uintptr_t*)(KLIB_MEMBER(5, gw, GameWorld_player, GW_PLAYER));
	if (PlausiblePtr(player))
	{
		uintptr_t cam = *(const uintptr_t*)(KLIB_MEMBER(5, player, PlayerInterface_camera, PLAYER_CAMERA));
		if (PlausiblePtr(cam))
			g_camAlt = *(const float*)(KLIB_MEMBER(5, cam, CameraClass_altitude, CAMERA_ALTITUDE));
		players = *(const int*)(KLIB_MEMBER(5, player, PlayerInterface_playerCharacters_count, 0x2B8));
	}
	s_players = players;

	if (!g_headerDone)
	{
		AuditLine("[AUDIT-HDR] " + SystemString());
		g_headerDone = true;
	}
	// Cursor split and PhysX owner sampling share this module check.
	// PhysXCore64.dll loads with the scene, after startPlugin.
	if ((g_cursor.splitOn || g_cfg.physxDetail) && !g_cursor.physxCore)
	{
		g_cursor.physxCore = GetModuleHandleA("PhysXCore64.dll");
		if (g_cursor.physxCore)
			AuditLine(Fmt("[Audit] PhysXCore64.dll at %p", (void*)g_cursor.physxCore));
	}
	if (g_cursor.splitOffWhy && !g_cursor.splitOffLogged)
	{
		AuditLine(std::string("[Audit] cursor split off: ") + g_cursor.splitOffWhy);
		g_cursor.splitOffLogged = true;
	}
	if (g_cursor.lockLayoutRejected && !g_cursor.lockRejectLogged)
	{
		AuditLine(Fmt("[Audit] PhysX lock layout rejected at %p", (void*)g_cursor.lockRejectedAt));
		g_cursor.lockRejectLogged = true;
	}
	std::string settings = SettingsString();
	if (settings != g_lastSettings)
	{
		AuditLine("[AUDIT-HDR] settings " + settings);
		g_lastSettings = settings;
	}
	if (g_cfg.offMainDetail)
		OffMainOncePerSecond();
}

void SampleWorld()
{
	uintptr_t gw = KlibAddress(g_base, RVA_GAMEWORLD);
	g_cur.chars = (int)*(const size_t*)(KLIB_MEMBER(5, gw, GameWorld_charUpdateListMain_size, GW_CHAR_COUNT));
	g_cur.dead  = (int)*(const size_t*)(KLIB_MEMBER(5, gw, GameWorld_deathParade_size, GW_DEAD_COUNT));
	g_cur.full  = *(const int*)(g_base + RVA_FULLRATE_COUNT);

	uintptr_t zm = *(const uintptr_t*)(KLIB_MEMBER(5, gw, GameWorld_zoneMgr, GW_ZONEMGR));
	if (PlausiblePtr(zm))
	{
		g_cur.setB      = (int)*(const size_t*)(KLIB_MEMBER(5, zm, ZoneManager_activeZones_size, ZM_SETB_COUNT));
		g_cur.zoneState = *(const int*)(KLIB_MEMBER(5, zm, ZoneManager_loadingPhase, ZM_STATE));

		// One 64-zone grid row per frame; the total refreshes every 64 frames.
		int loaded = 0;
		uintptr_t z = KlibZoneEntry(zm, g_zoneRow * ZONE_GRID);
		for (int j = 0; j < ZONE_GRID; ++j, z += ZONE_STRIDE)
		{
			if (*(const unsigned char*)(KLIB_MEMBER(5, z, ZoneMap_stateT_mainThreadData__zoneBeingLoaded, ZONE_LOADING)) || *(const unsigned char*)(KLIB_MEMBER(5, z, ZoneMap_stateT_mainThreadData__zoneIsLoaded, ZONE_ACCESS)))
				++loaded;
		}
		g_rowLoaded[g_zoneRow] = loaded;
		if (++g_zoneRow >= ZONE_GRID)
		{
			g_zoneRow = 0;
			int sum = 0;
			for (int i = 0; i < ZONE_GRID; ++i)
				sum += g_rowLoaded[i];
			g_zLoaded = sum;
		}
	}

	if (g_cur.M1 - g_last1Hz >= g_qpcFreq)
	{
		g_last1Hz = g_cur.M1;
		OncePerSecond();
	}
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
