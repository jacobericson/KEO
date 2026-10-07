// audit_render.cpp - Frame and render detours.
// Main thread records frame and draw state; other callers pass through (an Ogre worker at the barrier records its thread id).
// The boundary fallback queues a notice under g_lineCS alone; timing takes no locks.

#include "audit_detail.h"
#include "audit_steady.h"
#include "audit_offmain.h"

namespace kenshiframeaudit_detail {

// =========================================================================
// Entry detours
// =========================================================================

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

RenderPhase02_t oRenderPhase02 = NULL;
CullPhase01_t   oCullPhase01   = NULL;
VoidThis_t      oRenderVisible = NULL;
Rso_t           oRso           = NULL;
SetPass_t       oSetPass       = NULL;

RenderOneFrame_t oRenderOneFrame = NULL;
FrameListener_t  oFrameStarted   = NULL;
FrameListener_t  oFrameQueued    = NULL;
FrameListener_t  oFrameEnded     = NULL;
VoidThis_t       oUpdateScene    = NULL;
VoidThis_t       oCm2Update      = NULL;
VoidThis_t       oCm2Swap        = NULL;
RsRender_t       oRsRender       = NULL;
ThreadBody_t     oAiBody         = NULL;
ThreadBody_t     oPhysBody       = NULL;
ThreadBody_t     oBirdsBody      = NULL;
VoidThis_t       oPhysUT         = NULL;
ZoneLifecycle_t  oZoneLifecycle  = NULL;

bool hk_RenderOneFrame(void* root)
{
	if (!g_boundaryR || !IsMain())
		return oRenderOneFrame(root);

	if (g_rofDepth == 0)
		FrameBoundary(Now(), true);
	++g_rofDepth;
	bool r = oRenderOneFrame(root);
	--g_rofDepth;
	if (g_rofDepth == 0 && g_cur.open)
	{
		g_cur.R1  = Now();
		g_cur.hR1 = true;
	}
	return r;
}

bool hk_FrameStarted(void* self, const void* evt)
{
	if (!g_installed || !IsMain())
		return oFrameStarted(self, evt);
	Listeners_OnFrameStarted();
	Hulls_OnFrameStarted();

	LONGLONG t0 = Now();
	if (g_boundaryR && (!g_cur.open || g_cur.hT0))
	{
		// frameStarted without a renderOneFrame in between: renderOneFrame is
		// not driving the frames, so fall back to framing on frameStarted.
		g_boundaryR = false;
		AuditLine("[Audit] renderOneFrame not framing the loop; framing on frameStarted");
	}
	if (!g_boundaryR)
		FrameBoundary(t0, false);

	g_cur.T0  = t0;
	g_cur.hT0 = true;
	bool r = oFrameStarted(self, evt);
	g_cur.T1  = Now();
	g_cur.hT1 = true;
	return r;
}

bool hk_FrameQueued(void* self, const void* evt)
{
	if (!IsMain() || !g_cur.open)
		return oFrameQueued(self, evt);
	g_cur.Q0  = Now();
	g_cur.hQ0 = true;
	bool r = oFrameQueued(self, evt);
	g_cur.Q1  = Now();
	g_cur.hQ1 = true;
	return r;
}

bool hk_FrameEnded(void* self, const void* evt)
{
	if (!IsMain() || !g_cur.open)
		return oFrameEnded(self, evt);
	g_cur.E0  = Now();
	g_cur.hE0 = true;
	bool r = oFrameEnded(self, evt);
	g_cur.E1  = Now();
	g_cur.hE1 = true;
	return r;
}

void hk_UpdateScene(void* sm)
{
	if (!IsMain() || !g_cur.open)
	{
		oUpdateScene(sm);
		return;
	}
	LONGLONG t0 = Now();
	oUpdateScene(sm);
	LONGLONG d = Now() - t0;
	if (g_cur.cmDepth > 0)
	{
		g_cur.sgNested += d;
		g_cur.flags |= F_SGNESTED;
	}
	else
		g_cur.sgTop += d;
}

void hk_Cm2Update(void* cm)
{
	if (!IsMain() || !g_cur.open)
	{
		oCm2Update(cm);
		return;
	}
	++g_cur.cmDepth;
	LONGLONG t0 = Now();
	oCm2Update(cm);
	g_cur.cmTicks += Now() - t0;
	--g_cur.cmDepth;
}

void hk_Cm2Swap(void* cm)
{
	if (!IsMain() || !g_cur.open)
	{
		oCm2Swap(cm);
		return;
	}
	LONGLONG t0 = Now();
	oCm2Swap(cm);
	g_cur.swTicks += Now() - t0;
}

void hk_RsRender(void* rs, const void* op)
{
	if (g_installed && IsMain() && g_cur.open)
	{
		++g_cur.draws;
		bool shadow = g_getStage && g_sceneMgr && g_getStage(g_sceneMgr) == OGRE_STAGE_TEXTURE_SHADOWS;
		if (shadow)
			++g_cur.shadowDraws;
		size_t inst = op ? *(const size_t*)(KLIB_MEMBER(5, (const char*)op, Ogre__RenderOperation_numberOfInstances, OP_NUM_INSTANCES)) : 1;
		if (inst == 0 || inst > 1000000)
			inst = 1;
		if (inst > 1)
		{
			++g_cur.instDraws;
			g_cur.instances += (int)inst;
		}
		if (g_renderOn)
		{
			if (g_depth > 0)
			{
				int idx = g_stack[g_depth - 1].idx;
				if (idx >= 0)
				{
					RCall& rc = g_cur.calls[idx];
					Inc16(rc.draws);
					if (inst > 1) Inc16(rc.instDraws);
					rc.instances += (unsigned)inst;
					if (g_rsoDepth == 0) Inc16(rc.noRso);
				}
				if (g_rsoDepth == 0)
					++g_cur.drawsNoRso;
			}
			else
				++g_cur.drawsOut;
			if (g_rsoDepth > 0)
			{
				int s = shadow ? 1 : 0;
				++g_clsDraws[s][g_rsoCls];
				g_clsInst[s][g_rsoCls] += (LONG)inst;
				if (g_rsoMesh > 0)
				{
					++g_meshDraws[s][g_rsoMesh];
					if (g_rsoBatch) ++g_meshBatch[s][g_rsoMesh];
					g_meshInst[s][g_rsoMesh] += (LONG)inst;
				}
			}
		}
	}
	oRsRender(rs, op);
}

inline bool RenderTracking()
{
	return g_renderOn && IsMain() && g_cur.open;
}

// Nested scene time is charged to the enclosing call's "child" (and to its
// "rvoChild" when it happened inside that call's _renderVisibleObjects).
inline void ChargeParent(LONGLONG ticks)
{
	if (g_depth > 0)
	{
		CallCtx& p = g_stack[g_depth - 1];
		p.child += ticks;
		if (p.rvoT0)
			p.rvoChild += ticks;
	}
}

float TakePendingCull(const void* cam, const void* vp)
{
	for (int i = 0; i < g_npending; ++i)
	{
		if (g_pending[i].cam == cam && g_pending[i].vp == vp)
		{
			float ms = g_pending[i].ms;
			g_pending[i] = g_pending[--g_npending];
			return ms;
		}
	}
	return 0.0f;
}

void hk_RenderPhase02(void* sm, void* cam, const void* lod, void* vp,
                      unsigned char firstRq, unsigned char lastRq, bool overlays)
{
	if (!RenderTracking() || g_depth >= MAX_DEPTH)
	{
		oRenderPhase02(sm, cam, lod, vp, firstRq, lastRq, overlays);
		return;
	}
	bool shadow = g_getStage && g_getStage(sm) == OGRE_STAGE_TEXTURE_SHADOWS;
	int idx = -1;
	if (g_cur.ncalls < MAX_RCALLS)
	{
		idx = g_cur.ncalls++;
		RCall& rc = g_cur.calls[idx];
		memset(&rc, 0, sizeof(rc));
		rc.cam     = (unsigned char)CameraId(cam);
		rc.vp      = (unsigned char)ViewportId(vp);
		rc.firstRq = firstRq;
		rc.lastRq  = lastRq;
		rc.flags   = (unsigned char)((shadow ? RC_SHADOW : 0) | (g_depth > 0 ? RC_NESTED : 0));
		rc.cullMs  = TakePendingCull(cam, vp);
	}
	else
		g_cur.flags |= F_RCALLSFULL;
	++g_cur.sceneCalls;

	int depth0 = g_depth;
	CallCtx& c = g_stack[g_depth++];
	memset(&c, 0, sizeof(c));
	c.idx = idx;
	c.t0  = Now();
	oRenderPhase02(sm, cam, lod, vp, firstRq, lastRq, overlays);
	LONGLONG total = Now() - c.t0;
	g_depth = depth0;   // restore, never decrement blindly

	if (idx >= 0)
	{
		RCall& rc = g_cur.calls[idx];
		rc.ms       = TicksToMs(total - c.child);
		rc.submitMs = TicksToMs(c.rvoTicks - c.rvoChild);
		double perMs = g_tscPerMs;
		rc.rsoMs     = perMs > 0.0 ? (float)((double)c.rsoTsc / perMs) : Nan();
		rc.setPassMs = perMs > 0.0 ? (float)((double)c.passTsc / perMs) : Nan();
		rc.bindMs    = (perMs > 0.0 && g_bindHooked) ? (float)((double)c.bindTsc / perMs) : Nan();
		rc.d3dMs     = (perMs > 0.0 && g_d3dHooked) ? (float)((double)c.d3dTsc / perMs) : Nan();
		rc.syncMs    = g_syncHooked ? TicksToMs(c.syncTicks) : Nan();
	}
	ChargeParent(total);
}

void hk_CullPhase01(void* sm, void* cam, const void* lod, void* vp,
                    unsigned char firstRq, unsigned char lastRq)
{
	if (!RenderTracking())
	{
		oCullPhase01(sm, cam, lod, vp, firstRq, lastRq);
		return;
	}
	LONGLONG t0 = Now();
	oCullPhase01(sm, cam, lod, vp, firstRq, lastRq);
	LONGLONG d = Now() - t0;
	ChargeParent(d);

	float ms = TicksToMs(d);
	for (int i = 0; i < g_npending; ++i)
	{
		if (g_pending[i].cam == cam && g_pending[i].vp == vp)
		{
			g_pending[i].ms += ms;   // culled twice before rendering
			return;
		}
	}
	if (g_npending < MAX_PENDING)
	{
		PendingCull& p = g_pending[g_npending++];
		p.cam     = cam;
		p.vp      = vp;
		p.camId   = (unsigned char)CameraId(cam);
		p.vpId    = (unsigned char)ViewportId(vp);
		p.ms      = ms;
		p.firstRq = firstRq;
		p.lastRq  = lastRq;
		p.shadow  = (unsigned char)(g_getStage && g_getStage(sm) == OGRE_STAGE_TEXTURE_SHADOWS);
	}
}

void hk_RenderVisible(void* sm)
{
	if (!RenderTracking() || g_depth <= 0 || g_depth > MAX_DEPTH || g_stack[g_depth - 1].rvoT0 != 0)
	{
		oRenderVisible(sm);
		return;
	}
	CallCtx& c = g_stack[g_depth - 1];
	c.rvoT0 = Now();
	oRenderVisible(sm);
	c.rvoTicks += Now() - c.rvoT0;
	c.rvoT0 = 0;
}

void hk_Rso(void* sm, void* rend, const void* pass, bool scissor, bool lights)
{
	if (!RenderTracking() || !rend)
	{
		oRso(sm, rend, pass, scissor, lights);
		return;
	}
	int cls = ClassIdOf(*(const void* const*)rend);
	int mesh = 0;
	bool batch = false;
	bool attach = false;
	const NameEntry& ce = g_clsE[cls];
	if (ce.kind != KIND_OTHER)
	{
		const char* obj = (const char*)rend - ce.objOffset;
		const void* sp = NULL;
		if (ce.kind == KIND_SUBENTITY)
		{
			const void* ent = g_subParent ? g_subParent(obj) : NULL;   // Entity: MovableObject at 0
			if (ent && g_entMesh)
				sp = g_entMesh(ent);
			if (ent && g_getVisFlags)
				attach = (g_getVisFlags(ent) & VIS_ATTACHMENTS) != 0;
		}
		else
		{
			batch = true;
			if (g_batchMesh)
				sp = g_batchMesh(obj);
		}
		const void* m = sp ? (const void*)KLIB_MESH_POINTER(sp) : NULL;   // SharedPtr<Mesh>::pRep
		if (m)
			mesh = MeshIdOf(m);
	}
	bool shadow = g_getStage && g_getStage(sm) == OGRE_STAGE_TEXTURE_SHADOWS;

	int  saveCls = g_rsoCls, saveMesh = g_rsoMesh;
	bool saveBatch = g_rsoBatch;
	g_rsoCls = cls; g_rsoMesh = mesh; g_rsoBatch = batch;
	++g_rsoDepth;
	unsigned long long t0 = __rdtsc();
	oRso(sm, rend, pass, scissor, lights);
	unsigned long long dt = __rdtsc() - t0;
	--g_rsoDepth;
	g_rsoCls = saveCls; g_rsoMesh = saveMesh; g_rsoBatch = saveBatch;

	g_clsTsc[shadow ? 1 : 0][cls] += (LONG64)dt;
	++g_cur.rsos;
	if (g_depth > 0)
	{
		CallCtx& c = g_stack[g_depth - 1];
		c.rsoTsc += dt;
		if (c.idx >= 0)
		{
			Inc16(g_cur.calls[c.idx].rsos);
			if (attach)
				Inc16(g_cur.calls[c.idx].attachRso);
		}
	}
}

// ---- Fixed per-pass costs and the D3D11 per-draw path (main thread) -------

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
BarrierSync_t oBarrierSync = NULL;
VoidThis_t    oOldAnims    = NULL;
D3DBind_t     oD3DBind     = NULL;
RsRender_t    oD3DRender   = NULL;

// Workers call this too (it is the fork/join): they pass straight through.
void hk_BarrierSync(void* barrier)
{
	// Ogre's worker threads reach the barrier too: the CPU sampler names them by it.
	if (g_cfg.cpuSample && !IsMain())
		CpuNoteOgreWorker();
	bool om = g_cfg.offMainDetail && IsMain();
	OgreSyncCall sc = { 0, 0, false, 0 };
	if (om)
		OgreSyncEnter(barrier, &sc);   // a fire's stamp is published before the original releases the workers
	if (!RenderTracking())
	{
		oBarrierSync(barrier);
		if (om)
			OgreSyncExit(sc);
		return;
	}
	LONGLONG t0 = Now();
	oBarrierSync(barrier);
	LONGLONG d = Now() - t0;
	if (om)
		OgreSyncExit(sc);
	g_cur.syncTicks += d;
	++g_cur.syncs;
	if (g_depth > 0)
		g_stack[g_depth - 1].syncTicks += d;
}

void hk_OldAnims(void* sm)
{
	if (!RenderTracking())
	{
		oOldAnims(sm);
		return;
	}
	LONGLONG t0 = Now();
	if (g_cfg.offMainDetail)
		OgreOldAnimsEnter();
	oOldAnims(sm);
	if (g_cfg.offMainDetail)
		OgreOldAnimsExit();
	g_cur.oldAnimTicks += Now() - t0;
	++g_cur.oldAnims;
}

// The params SharedPtr is passed by value (a pointer to the caller's copy,
// which the callee destroys): forwarded untouched.
void hk_D3DBind(void* rs, int type, void* params, unsigned __int64 mask)
{
	if (!RenderTracking())
	{
		oD3DBind(rs, type, params, mask);
		return;
	}
	unsigned long long t0 = __rdtsc();
	oD3DBind(rs, type, params, mask);
	unsigned long long dt = __rdtsc() - t0;
	g_cur.bindTsc += dt;
	++g_cur.binds;
	if (g_depth > 0)
		g_stack[g_depth - 1].bindTsc += dt;
}

void hk_D3DRender(void* rs, const void* op)
{
	if (!RenderTracking())
	{
		oD3DRender(rs, op);
		return;
	}
	unsigned long long t0 = __rdtsc();
	oD3DRender(rs, op);
	unsigned long long dt = __rdtsc() - t0;
	g_cur.d3dTsc += dt;
	if (g_depth > 0)
		g_stack[g_depth - 1].d3dTsc += dt;
}

const void* hk_SetPass(void* sm, const void* pass, bool evenIfSuppressed, bool shadowDerivation)
{
	if (!RenderTracking())
		return oSetPass(sm, pass, evenIfSuppressed, shadowDerivation);
	unsigned long long t0 = __rdtsc();
	const void* r = oSetPass(sm, pass, evenIfSuppressed, shadowDerivation);
	unsigned long long dt = __rdtsc() - t0;
	++g_cur.setPasses;
	if (g_depth > 0)
	{
		CallCtx& c = g_stack[g_depth - 1];
		c.passTsc += dt;
		if (c.idx >= 0)
			Inc16(g_cur.calls[c.idx].setPasses);
	}
	return r;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
