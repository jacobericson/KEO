#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/module_hooks.h"
#include "render/shadow_reach_math.h"
#include "render/shadow_receiver_depth.h"
#include "game/game.h"
#include "base/core.h"
#include <math.h>
#include <intrin.h>

// The shadow cascades' casters and passes. The cascade setup (exe) runs on
// the main thread once per cascade, and that cascade's cull and render
// follow on the same thread before the next setup. Its post-hook arms a slot
// with the cascade's reach planes (the region from which a caster can shadow
// a receiver of the cascade). The cull detour, on Ogre's worker threads,
// counts the casters against the planes (shadowReachDiag) and drops those
// that cannot reach (shadowReachCull); the workers are released by a barrier
// after the setup returns. The render-phase detour disarms the slot after
// the last cascade. One shadow camera serves every cascade, so the slot's
// cascade index is what tells them apart.
//
// Dropping a caster that cannot reach any receiver changes no stored depth a
// receiver compares against, so the image is unchanged by construction.

static const size_t CSM_COUNT   = 0x8;    // int: cascade count
static const size_t CSM_SPLITS  = 0x10;   // float*: count + 1 split depths
static const size_t SKY_SUN_DIR = 0xC;    // Vector3 toward the sun, y clamped >= 0

// PCF reaches about 1.4 texels around the receiver.
static const float TEXEL_MARGIN = 2.0f;
// The caster depth bias only moves stored depth away from the light; this
// is slack along the light direction on top of that.
static const float LIGHT_MARGIN = 10.0f;
// A skinned object's AABB can trail its animation.
static const float AABB_GROW    = 20.0f;
static const float BIG_EXTENT   = 500.0f;

static const int REACH_CASCADES = 4;
namespace shadow_reach_detail {
enum { R_IN, R_REACH, R_CUT, R_INF, R_BIG, R_FIELDS };
} // namespace shadow_reach_detail
using namespace shadow_reach_detail;
static volatile LONG s_counts[REACH_CASCADES][R_FIELDS];
static volatile LONG s_cullIn  = 0;
static volatile LONG s_cullCut = 0;

// Sun direction relative to the camera heading, centidegrees, from the last
// armed far cascade.
static volatile LONG s_sunElev = 0;
static volatile LONG s_sunAz   = 0;
static volatile LONG s_sunSeen = 0;

namespace shadow_reach_detail {
struct OgreFastArray
{
	void** data;
	size_t size;
	size_t capacity;
};

// ObjectData is passed by value, which the x64 ABI passes as a pointer to the
// caller's copy; the original advances that copy's pointers.
typedef void (*CullFrustum_t)(size_t numNodes, void* objData, const void* frustum,
                              unsigned visMask, OgreFastArray* out, const void* lodCamera);
typedef void (*SetupCascade_t)(void* csm, void* light, void* viewport, void* csmCamera,
                               void* mainCamera, int cascade);
typedef void (*RenderPhase_t)(void* target, void* viewport, void* camera, const void* lodCamera,
                              unsigned char firstRq, unsigned char lastRq, bool includeOverlays);
// Struct returns go through a hidden pointer in rdx (this in rcx).
typedef void* (*GetWorldAabb_t)(const void* obj, float* centerHalf6);
typedef float* (*CamVector_t)(const void* cam, float* out3);
typedef const float* (*CamVectorRef_t)(const void* cam);
typedef const float* (*FrustumFovY_t)(const void* frustum);   // Radian&
typedef float (*FrustumAspect_t)(const void* frustum);
typedef int (*ViewportWidth_t)(const void* viewport);
typedef void* (*ParentNode_t)(const void* movable);
typedef bool (*FrustumFlag_t)(const void* frustum);
typedef const float* (*FrustumOffset_t)(const void* frustum);   // Vector2&
} // namespace shadow_reach_detail
using namespace shadow_reach_detail;

static const char* const OGRE_MODULE = "OgreMain_x64.dll";

static const ModuleSite s_cullSite =
{
	"MovableObject::cullFrustum", "OgreMain_x64.dll",
	"?cullFrustum@MovableObject@Ogre@@SAX_KUObjectData@2@PEBVFrustum@2@IAEAV?$FastArray@PEAVMovableObject@Ogre@@@2@PEBVCamera@2@@Z",
	0x1D7A90,
	{ 0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xD8 }
};

static const ModuleSite s_renderPhaseSite =
{
	"RenderTarget::_updateViewportRenderPhase02", "OgreMain_x64.dll",
	"?_updateViewportRenderPhase02@RenderTarget@Ogre@@UEAAXPEAVViewport@2@PEAVCamera@2@PEBV42@EE_N@Z",
	0x26CCE0,
	{ 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x30,0x0F }
};

static CullFrustum_t   s_origCull        = NULL;
static SetupCascade_t  s_origSetup       = NULL;
static RenderPhase_t   s_origRenderPhase = NULL;
static GetWorldAabb_t  s_getWorldAabb    = NULL;
static CamVectorRef_t  s_camPosition     = NULL;
static CamVector_t     s_camDirection    = NULL;
static CamVector_t     s_camUp           = NULL;
static CamVector_t     s_camRight        = NULL;
static FrustumFovY_t   s_fovY            = NULL;
static FrustumAspect_t s_aspect          = NULL;
static ViewportWidth_t s_viewportWidth   = NULL;
static ParentNode_t    s_parentNode      = NULL;   // optional: the focus distance
static CamVector_t     s_nodePosition    = NULL;   // optional: the focus distance
// Optional: without them the camera's shape cannot be verified, so nothing is cut.
static FrustumFlag_t   s_customProj      = NULL;
static FrustumOffset_t s_frustumOffset   = NULL;

// The shared hooks install once, whichever lever asks first; each lever row
// then enables its own use of them.
static int  s_hooks          = 0;   // 0 not tried, 1 installed, -1 refused
static bool s_diagEnabled    = false;
static bool s_cullEnabled    = false;

static bool DiagActive() { return s_diagEnabled && g_renderCfg.shadowReachDiag; }
static bool CullActive() { return s_cullEnabled && g_renderCfg.shadowReachCull; }

// Main PublishSlot arms the shadow camera and reach planes with seq odd
// while writing, even when published. Ogre cull workers copy through one
// ReadSlot attempt; main render phase reads directly. Disarmed after the
// final cascade and on refused or disabled arm paths. A torn behavior input
// is rejected: the original caster list remains, costing only missed culling
// and diagnostic tallies for that call.
namespace shadow_reach_detail {
struct ReachSlot
{
	volatile LONG        seq;
	const void* volatile frustum;   // the armed shadow camera; NULL when disarmed
	int                  cascade;
	int                  cascades;
	int                  count;     // reach planes; -1 when they could not be built
	ReachPlane           planes[REACH_MAX_PLANES];
};
} // namespace shadow_reach_detail
using namespace shadow_reach_detail;
static ReachSlot s_slot;

static void PublishSlot(const void* frustum, int cascade, int cascades,
                        const ReachPlane* planes, int count)
{
	InterlockedIncrement(&s_slot.seq);
	s_slot.frustum = frustum;
	s_slot.cascade = cascade;
	s_slot.cascades = cascades;
	s_slot.count = count;
	for (int k = 0; k < count; ++k)
		s_slot.planes[k] = planes[k];
	InterlockedIncrement(&s_slot.seq);
}

static void Disarm()
{
	PublishSlot(NULL, -1, 0, NULL, -1);
}

static bool ReadSlot(const void* frustum, int* cascade, int* count, ReachPlane* planes)
{
	LONG seq = s_slot.seq;
	if (seq & 1)
		return false;
	_ReadBarrier();
	if (s_slot.frustum != frustum)
		return false;
	*cascade = s_slot.cascade;
	*count = s_slot.count;
	if (*count < 0 || *count > REACH_MAX_PLANES || *cascade < 0 || *cascade >= REACH_CASCADES)
		return false;
	for (int k = 0; k < *count; ++k)
		planes[k] = s_slot.planes[k];
	_ReadBarrier();
	return s_slot.seq == seq;
}

namespace shadow_reach_detail {
struct CasterJudge
{
	const ReachPlane* planes;
	int               count;
};
} // namespace shadow_reach_detail
using namespace shadow_reach_detail;

// Worker threads: getWorldAabb is a leaf read of the object's SoA box.
static ReachVerdict JudgeCaster(const void* obj, void* ctx)
{
	const CasterJudge* j = (const CasterJudge*)ctx;
	float box[6];
	s_getWorldAabb(obj, box);
	if (!AabbIsFinite(box, box + 3))
		return RV_KEEP_INFINITE;
	if (AabbOutsideAnyPlane(j->planes, j->count, box, box + 3, AABB_GROW))
		return RV_CUT;
	float h = box[3] > box[4] ? box[3] : box[4];
	if (box[5] > h) h = box[5];
	return 2.0f * h > BIG_EXTENT ? RV_KEEP_BIG : RV_KEEP;
}

// Worker threads: no allocation, lock or logging. Only this call's range
// [before, size) is touched, and only its size is written.
static void FilterCasters(const void* frustum, OgreFastArray* out, size_t before)
{
	int cascade = 0, count = 0;
	ReachPlane planes[REACH_MAX_PLANES];
	if (!ReadSlot(frustum, &cascade, &count, planes))
		return;
	void** data = out->data;
	size_t after = out->size;
	if (!data || after <= before)
		return;

	bool cull = CullActive();
	CasterJudge judge = { planes, count };
	ReachTally t;
	size_t end = FilterReach(data, before, after, cull, &JudgeCaster, &judge, &t);
	if (cull)
	{
		if (end != after)
			out->size = end;
		InterlockedExchangeAdd(&s_cullIn, t.in);
		InterlockedExchangeAdd(&s_cullCut, t.cut);
	}
	if (DiagActive())
	{
		volatile LONG* c = s_counts[cascade];
		InterlockedExchangeAdd(&c[R_IN], t.in);
		InterlockedExchangeAdd(&c[R_REACH], t.reach);
		InterlockedExchangeAdd(&c[R_CUT], t.cut);
		InterlockedExchangeAdd(&c[R_INF], t.inf);
		InterlockedExchangeAdd(&c[R_BIG], t.big);
	}
}

static void hook_CullFrustum(size_t numNodes, void* objData, const void* frustum,
                             unsigned visMask, OgreFastArray* out, const void* lodCamera)
{
	bool casters = frustum && frustum == s_slot.frustum && (CullActive() || DiagActive());
	bool depth = !casters && DiagActive() && ReceiverDepth_Watches(frustum);
	if (!casters && !depth)
	{
		s_origCull(numNodes, objData, frustum, visMask, out, lodCamera);
		return;
	}
	size_t before = out->size;
	s_origCull(numNodes, objData, frustum, visMask, out, lodCamera);
	if (casters)
		FilterCasters(frustum, out, before);
	else
		ReceiverDepth_Fold(frustum, out->data, before, out->size, (WorldAabbFn)s_getWorldAabb);
}

// Main thread. Called through the render target's vtable for every scene
// pass; anything but the armed shadow camera passes straight through.
static void hook_RenderPhase(void* target, void* viewport, void* camera, const void* lodCamera,
                             unsigned char firstRq, unsigned char lastRq, bool includeOverlays)
{
	if (!camera || camera != s_slot.frustum)
	{
		s_origRenderPhase(target, viewport, camera, lodCamera, firstRq, lastRq, includeOverlays);
		return;
	}
	int cascade = s_slot.cascade;
	s_origRenderPhase(target, viewport, camera, lodCamera, firstRq, lastRq, includeOverlays);
	// The render-phase instance culls of this cascade have run by now.
	if (cascade == s_slot.cascades - 1)
		Disarm();
}

static bool Finite3(const float v[3])
{
	return _finite(v[0]) && _finite(v[1]) && _finite(v[2]);
}

// The slice planes model a symmetric perspective frustum; a custom
// projection or a frustum offset breaks that, and the cascade is left uncut.
static bool SymmetricPerspective(const void* camera)
{
	if (!s_customProj || !s_frustumOffset || s_customProj(camera))
		return false;
	const float* offset = s_frustumOffset(camera);
	return offset[0] == 0.0f && offset[1] == 0.0f;
}

// The reach planes for cascade i, from the same main-camera state and light
// direction the setup just used. Returns the plane count, or -1 when any
// input is missing or implausible.
static int BuildReach(void* csm, void* viewport, void* mainCamera, int i, ReachPlane* out)
{
	int cascades = *(const int*)((const char*)csm + CSM_COUNT);
	if (i < 0 || i >= REACH_CASCADES || i >= cascades)
		return -1;
	const float* splits = *(const float* const*)((const char*)csm + CSM_SPLITS);
	uintptr_t sky = *(const uintptr_t*)GameAddr(RVA_SKY_INSTANCE);
	if (!splits || !sky)
		return -1;
	float nearDepth = splits[i], farDepth = splits[i + 1];
	if (!SymmetricPerspective(mainCamera))
	{
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			LogMsg("Render: shadowReachCull: main camera is not a symmetric perspective frustum (or it cannot be checked); no caster is cut");
		}
		return -1;
	}

	// The setup builds the shadow camera's depth so that the sky's sun vector
	// points toward the light.
	float L[3];
	if (!LightTravelDirection((const float*)(sky + SKY_SUN_DIR), L))
		return -1;

	ReachCamera cam;
	const float* pos = s_camPosition(mainCamera);
	for (int k = 0; k < 3; ++k)
		cam.pos[k] = pos[k];
	s_camDirection(mainCamera, cam.fwd);
	s_camUp(mainCamera, cam.up);
	s_camRight(mainCamera, cam.right);
	cam.tanHalfFovY = tanf(0.5f * *s_fovY(mainCamera));
	cam.aspect = s_aspect(mainCamera);
	int width = s_viewportWidth(viewport);
	if (!Finite3(cam.pos) || !Finite3(cam.fwd) || !Finite3(cam.up) || !Finite3(cam.right) ||
	    !(cam.tanHalfFovY > 0.0f) || !(cam.aspect > 0.0f) || width <= 0 ||
	    !(farDepth > nearDepth) || !_finite(farDepth))
		return -1;

	// The box is at least CascadeBoxSide wide and no axis of it exceeds the
	// frustum's diameter, so the larger of the two bounds its texel.
	float side = CascadeBoxSide(nearDepth, farDepth, cam.tanHalfFovY, cam.aspect);
	float diameter = FrustumDiameter(farDepth, cam.tanHalfFovY, cam.aspect);
	float texel = (side > diameter ? side : diameter) / (float)width;

	ReachPlane slice[6];
	BuildSlicePlanes(cam, nearDepth - CASCADE_DEPTH_MARGIN, farDepth + CASCADE_DEPTH_MARGIN,
	                 TEXEL_MARGIN * texel, slice);
	int count = ExtrudeTowardLight(slice, 6, L, out, REACH_MAX_PLANES);
	if (count < 0)
		return -1;
	ShiftPlanes(out, count, L, LIGHT_MARGIN);
	for (int k = 0; k < count; ++k)
	{
		if (!Finite3(out[k].n) || !_finite(out[k].c))
			return -1;
	}

	if (i == cascades - 1)
	{
		float elev = 0.0f, az = 0.0f;
		SunElevationAzimuth(L, cam.fwd, cam.right, &elev, &az);
		InterlockedExchange(&s_sunElev, (LONG)floorf(elev * 100.0f + 0.5f));
		InterlockedExchange(&s_sunAz, (LONG)floorf(az * 100.0f + 0.5f));
		InterlockedExchange(&s_sunSeen, 1);
	}
	return count;
}

// The orbit camera sits on a child node of its focus point, so that node's
// offset is the focus distance; negative when unknown.
static float FocusDistance(const void* mainCamera)
{
	if (!s_parentNode || !s_nodePosition)
		return -1.0f;
	void* node = s_parentNode(mainCamera);
	if (!node)
		return -1.0f;
	float p[3];
	s_nodePosition(node, p);
	float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	return _finite(d) ? d : -1.0f;
}

static void hook_SetupCascade(void* csm, void* light, void* viewport, void* csmCamera,
                              void* mainCamera, int cascade)
{
	s_origSetup(csm, light, viewport, csmCamera, mainCamera, cascade);
	bool diag = DiagActive(), cull = CullActive();
	if (!diag && !cull)
		return;
	int cascades = *(const int*)((const char*)csm + CSM_COUNT);
	const float* splits = *(const float* const*)((const char*)csm + CSM_SPLITS);
	if (diag && cascade == 0)
		ReceiverDepth_OnShadowStart(splits, cascades, mainCamera, FocusDistance(mainCamera));
	if (cascade < 0 || cascade >= REACH_CASCADES || cascade >= cascades || !splits)
	{
		Disarm();
		return;
	}
	ReachPlane planes[REACH_MAX_PLANES];
	int count = BuildReach(csm, viewport, mainCamera, cascade, planes);
	PublishSlot(csmCamera, cascade, cascades, planes, count);
}

static void DrainCounts(LONG out[REACH_CASCADES][R_FIELDS])
{
	for (int c = 0; c < REACH_CASCADES; ++c)
	{
		for (int f = 0; f < R_FIELDS; ++f)
			out[c][f] = InterlockedExchange(&s_counts[c][f], 0);
	}
}

// Main thread, outside the render passes. The slot is disarmed every tick
// (each cascade setup re-arms it), and each switch of the diagnostic zeroes
// its counters, the sun reading and the depth fold, so no window mixes both
// sides of a switch.
void ShadowReach_MainThreadTick()
{
	static int diagWas = -1;   // -1 before the first tick
	if (s_hooks != 1)
		return;
	if (s_slot.frustum)
		Disarm();

	int diagOn = DiagActive() ? 1 : 0;
	if (diagOn == diagWas)
		return;
	bool first = diagWas < 0;
	diagWas = diagOn;
	if (first)
		return;
	LONG drained[REACH_CASCADES][R_FIELDS];
	DrainCounts(drained);
	InterlockedExchange(&s_sunSeen, 0);
	ReceiverDepth_Reset();
}

std::string ShadowReachStatsToken(double windowSec)
{
	if (s_hooks != 1)
		return std::string();
	LONG counts[REACH_CASCADES][R_FIELDS];
	DrainCounts(counts);
	LONG cullIn = InterlockedExchange(&s_cullIn, 0);
	LONG cullCut = InterlockedExchange(&s_cullCut, 0);
	bool diag = DiagActive();
	std::string depth;
	if (diag)
		depth = ReceiverDepthToken(windowSec);
	else
		ReceiverDepth_ClearWindow();

	double perSec = windowSec > 0.0 ? 1.0 / windowSec : 0.0;
	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(0);
	if (CullActive() || cullIn > 0)
		ss << " reachCut=" << (double)cullCut * perSec << "/" << (double)cullIn * perSec;
	if (!diag)
		return ss.str();
	ss << " reach=";
	for (int c = REACH_CASCADES - 1; c >= 0; --c)
	{
		ss << "c" << c << ":";
		for (int f = 0; f < R_FIELDS; ++f)
			ss << (f ? "/" : "") << (double)counts[c][f] * perSec;
		if (c > 0)
			ss << ",";
	}
	if (s_sunSeen)
		ss << " sun=el" << (double)s_sunElev / 100.0 << "/az" << (double)s_sunAz / 100.0;
	else
		ss << " sun=-";
	ss << depth;
	return ss.str();
}

static bool Resolve(HMODULE ogre, const char* symbol, void** out)
{
	*out = (void*)GetProcAddress(ogre, symbol);
	if (*out)
		return true;
	LogMsg(std::string("Render: shadow hooks: OgreMain export missing: ") + symbol);
	return false;
}

// The cascade-setup arm, the cull filter and the render-phase detour that
// disarms after the last cascade. Installed once, whichever lever asks first.
static bool InstallShadowHooks()
{
	if (s_hooks != 0)
		return s_hooks == 1;
	s_hooks = -1;
	HMODULE ogre = GetModuleHandleA(OGRE_MODULE);
	if (!ogre)
	{
		LogMsg("Render: shadow hooks: OgreMain not loaded");
		return false;
	}
	if (!Resolve(ogre, "?getWorldAabb@MovableObject@Ogre@@QEBA?AUAabb@2@XZ", (void**)&s_getWorldAabb) ||
	    !Resolve(ogre, "?getDerivedPosition@Camera@Ogre@@QEBAAEBVVector3@2@XZ", (void**)&s_camPosition) ||
	    !Resolve(ogre, "?getDerivedDirection@Camera@Ogre@@QEBA?AVVector3@2@XZ", (void**)&s_camDirection) ||
	    !Resolve(ogre, "?getDerivedUp@Camera@Ogre@@QEBA?AVVector3@2@XZ", (void**)&s_camUp) ||
	    !Resolve(ogre, "?getDerivedRight@Camera@Ogre@@QEBA?AVVector3@2@XZ", (void**)&s_camRight) ||
	    !Resolve(ogre, "?getFOVy@Frustum@Ogre@@UEBAAEBVRadian@2@XZ", (void**)&s_fovY) ||
	    !Resolve(ogre, "?getAspectRatio@Frustum@Ogre@@UEBAMXZ", (void**)&s_aspect) ||
	    !Resolve(ogre, "?getActualWidth@Viewport@Ogre@@QEBAHXZ", (void**)&s_viewportWidth))
		return false;
	s_parentNode = (ParentNode_t)GetProcAddress(ogre, "?getParentSceneNode@MovableObject@Ogre@@QEBAPEAVSceneNode@2@XZ");
	s_nodePosition = (CamVector_t)GetProcAddress(ogre, "?getPosition@Node@Ogre@@QEBA?AVVector3@2@XZ");
	s_customProj = (FrustumFlag_t)GetProcAddress(ogre, "?isCustomProjectionMatrixEnabled@Frustum@Ogre@@UEBA_NXZ");
	s_frustumOffset = (FrustumOffset_t)GetProcAddress(ogre, "?getFrustumOffset@Frustum@Ogre@@UEBAAEBVVector2@2@XZ");
	if (!VerifyPrologueByRva(RVA_CSM_SETUP_CASCADE))
		return false;
	// The detours go in before the arm: they do nothing until the arm fills
	// the slot, so a refused arm leaves them inert.
	if (!InstallModuleHook(s_cullSite, (void*)hook_CullFrustum, (void**)&s_origCull, NULL))
		return false;
	if (!InstallModuleHook(s_renderPhaseSite, (void*)hook_RenderPhase, (void**)&s_origRenderPhase, NULL))
		return false;
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(GameAddr(RVA_CSM_SETUP_CASCADE), hook_SetupCascade, &s_origSetup))
	{
		LogMsg("Render: shadow hooks: AddHook failed on CsmShadowMap::setupCascade");
		return false;
	}
	s_hooks = 1;
	return true;
}

bool InstallShadowReach()
{
	s_diagEnabled = InstallShadowHooks();
	return s_diagEnabled;
}

bool InstallShadowReachCull()
{
	s_cullEnabled = InstallShadowHooks();
	return s_cullEnabled;
}
