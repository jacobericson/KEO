// audit_d3d.cpp - The scene and render probes' D3D11 half (SceneDetail=1), on the main thread: the
// render system's state flags read around each draw, outside the draw's timing (how often a
// flagged blend, rasterizer or depth-stencil state's description equals the one the still-bound
// object was made from), and three call-site rows in RenderSystem_Direct3D11_x64.dll: the blend and
// sampler state creations and the render-target change's ClearState, which also forgets the
// remembered descriptions. The rows go in only for the module build the offsets were read from.
// No hook or callback takes a lock, allocates, or logs.

#include "audit_scene.h"
#include "audit_offmain.h"
#include "audit_scene_rules.h"

namespace audit_d3d_detail {

// The description a state's bound object was last made from.
struct Shadow
{
	unsigned char desc[264];
	unsigned      ref;    // depth-stencil: the stencil reference bound with it
	bool          held;
};

// One render-system state: its changed flag (a byte), its description and size, its bound object.
struct StateLayout
{
	size_t changed, desc, size, bound;
};

// Main-thread totals of the frame in flight.
struct D3dFrame
{
	LONGLONG blendTicks, sampTicks;
	int      blendN, sampN, clearN, smpChg;
	int      chg[3], same[3];   // blend, rasterizer, depth-stencil
};

} // audit_d3d_detail
using namespace audit_d3d_detail;

namespace kenshiframeaudit_detail {

using CallSiteProbe::SHAPE_INT;

// The RenderSystem_Direct3D11_x64.dll build every offset here was read from.
static const ModuleBuild D3D11_BUILD = { 0x5CA5F9E9, 0xBF000, { "dllStartPlugin", "dllStopPlugin" }, { 0x4D30, 0x4D80 } };

// D3D11RenderSystem, as _render reads it: a flagged state is made again from its description and
// rebound; an unflagged one keeps its bound object.
static const size_t RS_BLEND_CHANGED    = 0x5B0;
static const size_t RS_BLEND_DESC       = 0x4A8;   // 264 bytes
static const size_t RS_BLEND_BOUND      = 0x738;
static const size_t RS_RASTER_CHANGED   = 0x5DC;
static const size_t RS_RASTER_DESC      = 0x5B4;   // 40 bytes
static const size_t RS_RASTER_BOUND     = 0x740;
static const size_t RS_DEPTH_CHANGED    = 0x618;
static const size_t RS_DEPTH_DESC       = 0x5E4;   // 52 bytes
static const size_t RS_DEPTH_BOUND      = 0x748;
static const size_t RS_STENCIL_REF      = 0x5E0;   // u32, passed to OMSetDepthStencilState
static const size_t RS_SAMPLERS_CHANGED = 0xF20;

enum { STATE_BLEND, STATE_RASTER, STATE_DEPTH, NUM_STATES };
static const StateLayout s_states[NUM_STATES] =
{
	{ RS_BLEND_CHANGED,  RS_BLEND_DESC,  264, RS_BLEND_BOUND },
	{ RS_RASTER_CHANGED, RS_RASTER_DESC, 40,  RS_RASTER_BOUND },
	{ RS_DEPTH_CHANGED,  RS_DEPTH_DESC,  52,  RS_DEPTH_BOUND },
};

// Call sites in RenderSystem_Direct3D11_x64.dll (RVAs from its image base); the two creations load
// the device's vtable two instructions before the call.
static CallSiteProbe::Site s_d3dSites[] =
{
	{ "d3dBlendCreate", 0x342C2, 0, SHAPE_INT, ST_D3D_BLEND, 0xA0,  0, 1 },  // ID3D11Device::CreateBlendState
	{ "d3dSampCreate",  0x3467D, 0, SHAPE_INT, ST_D3D_SAMP,  0xB8,  0, 1 },  // ID3D11Device::CreateSamplerState
	{ "d3dClearState",  0x33726, 0, SHAPE_INT, ST_D3D_CLEAR, 0x370, 0, 0 },  // ID3D11DeviceContext::ClearState
};
extern const int NUM_D3D_SITES = sizeof(s_d3dSites) / sizeof(s_d3dSites[0]);

// Main thread.
static Shadow   s_shadow[NUM_STATES];
static bool     s_stateOn = false;
static D3dFrame s_fr;

const char* InstallD3dProbes(HMODULE d3d, int* rows)
{
	s_stateOn = false;
	*rows = 0;
	if (!d3d)
		return "no module";
	if (!ModuleMatches(d3d, D3D11_BUILD))
		return "build mismatch";
	int idx[NUM_D3D_SITES];
	for (int i = 0; i < NUM_D3D_SITES; ++i)
		idx[i] = i;
	int ok = InstallOffMainRows(d3d, s_d3dSites, idx, NUM_D3D_SITES);
	MarkOffMainTags(s_d3dSites, NUM_D3D_SITES);
	*rows = ok;
	// The remembered descriptions are only right while ClearState forgets them.
	s_stateOn = g_d3dHooked && g_haveTag[ST_D3D_CLEAR];
	return ok == NUM_D3D_SITES ? NULL : (ok > 0 ? "partial" : "no row installed");
}

bool D3dStateOn()
{
	return s_stateOn;
}

void D3dStateEnter(void* rs, D3dFlags* f)
{
	const unsigned char* p = (const unsigned char*)rs;
	f->blend   = p[RS_BLEND_CHANGED];
	f->raster  = p[RS_RASTER_CHANGED];
	f->depth   = p[RS_DEPTH_CHANGED];
	f->sampler = p[RS_SAMPLERS_CHANGED];
	if (!g_cur.open)
		return;
	if (f->sampler)
		++s_fr.smpChg;
	const unsigned char flagged[NUM_STATES] = { f->blend, f->raster, f->depth };
	for (int i = 0; i < NUM_STATES; ++i)
	{
		if (!flagged[i])
			continue;
		const StateLayout& st = s_states[i];
		const Shadow& sh = s_shadow[i];
		++s_fr.chg[i];
		bool bound = *(const uintptr_t*)(p + st.bound) != 0;
		bool desc  = memcmp(p + st.desc, sh.desc, st.size) == 0;
		bool ref   = i == STATE_DEPTH ? *(const unsigned*)(p + RS_STENCIL_REF) == sh.ref : true;
		if (scenerules::WouldSkip(true, sh.held, bound, desc, ref))
			++s_fr.same[i];
	}
}

// After the draw: each flagged state's object is now bound, made from the description it read.
void D3dStateExit(void* rs, const D3dFlags& f)
{
	const unsigned char* p = (const unsigned char*)rs;
	const unsigned char flagged[NUM_STATES] = { f.blend, f.raster, f.depth };
	for (int i = 0; i < NUM_STATES; ++i)
	{
		if (!flagged[i])
			continue;
		const StateLayout& st = s_states[i];
		Shadow& sh = s_shadow[i];
		memcpy(sh.desc, p + st.desc, st.size);
		if (i == STATE_DEPTH)
			sh.ref = *(const unsigned*)(p + RS_STENCIL_REF);
		sh.held = true;
	}
}

void D3dProbeExit(int tag, LONGLONG t0, LONGLONG t1)
{
	bool open = g_cur.open;
	switch (tag)
	{
	case ST_D3D_CLEAR:
		for (int i = 0; i < NUM_STATES; ++i)
			s_shadow[i].held = false;
		if (open)
			++s_fr.clearN;
		break;
	case ST_D3D_BLEND:
		if (open)
		{
			s_fr.blendTicks += t1 - t0;
			++s_fr.blendN;
		}
		break;
	case ST_D3D_SAMP:
		if (open)
		{
			s_fr.sampTicks += t1 - t0;
			++s_fr.sampN;
		}
		break;
	default:
		break;
	}
}

void D3dFrameTotals(FrameRec& r)
{
	r.m[M_SC_D3DBLEND] = g_haveTag[ST_D3D_BLEND] ? TicksToMs(s_fr.blendTicks) : Nan();
	r.m[M_SC_D3DSAMP]  = g_haveTag[ST_D3D_SAMP] ? TicksToMs(s_fr.sampTicks) : Nan();
	r.c[C_D3DBLENDN] = s_fr.blendN;
	r.c[C_D3DSAMPN]  = s_fr.sampN;
	r.c[C_D3DCLEARN] = s_fr.clearN;
	r.c[C_BSCHGN]    = s_fr.chg[STATE_BLEND];
	r.c[C_BSSAMEN]   = s_fr.same[STATE_BLEND];
	r.c[C_RSCHGN]    = s_fr.chg[STATE_RASTER];
	r.c[C_RSSAMEN]   = s_fr.same[STATE_RASTER];
	r.c[C_DSCHGN]    = s_fr.chg[STATE_DEPTH];
	r.c[C_DSSAMEN]   = s_fr.same[STATE_DEPTH];
	r.c[C_SMPCHGN]   = s_fr.smpChg;
	memset(&s_fr, 0, sizeof(s_fr));
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
