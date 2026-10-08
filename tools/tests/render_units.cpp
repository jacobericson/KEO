#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include "base/worker_count.h"
#include "render/pe_imports.h"
#include "render/pu_layout.h"
#include "render/name_list.h"
#include "render/particle_policy.h"
#include "render/shadow_reach_math.h"
#include "render/render_keys.h"
#include "render/ptr_set.h"
#include "render/gpu_param_cache.h"
#include "render/id_string.h"
#include "render/empty_pass_policy.h"
#include "render/compositor_layout.h"
#include "render/pass_visibility.h"
#include "render/foliage_budget.h"
#include "render/upload_shadow.h"
#include "render/upload_plan.h"
#include "base/ini_text.h"
#include "gui/settings_rows.h"
#include "gui/settings_factory.h"
#include "base/config_table.h"
#include "navmesh/navmesh_config.h"
#include <vector>
#include <math.h>
#include <emmintrin.h>

#include "check.h"

// The bench units the config table's custom rows call need these from the
// runtime; the settings rows never reach them.
bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

static float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static bool Near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static bool SamePlane(const ReachPlane& a, const ReachPlane& b)
{
	return a.n[0] == b.n[0] && a.n[1] == b.n[1] && a.n[2] == b.n[2] && a.c == b.c;
}

// Camera at the origin looking down -z (Ogre's convention), 45 degree fovY,
// aspect 2, slice from depth 100 to 200.
static ReachCamera TestCamera()
{
	ReachCamera cam;
	const float pos[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, -1 }, up[3] = { 0, 1, 0 }, right[3] = { 1, 0, 0 };
	for (int k = 0; k < 3; ++k)
	{
		cam.pos[k] = pos[k]; cam.fwd[k] = fwd[k]; cam.up[k] = up[k]; cam.right[k] = right[k];
	}
	cam.tanHalfFovY = tanf(22.5f * 3.14159265f / 180.0f);
	cam.aspect = 2.0f;
	return cam;
}

// Extruded region for the test slice; returns the plane count.
static int TestReach(const float L[3], ReachPlane out[REACH_MAX_PLANES])
{
	ReachPlane slice[6];
	BuildSlicePlanes(TestCamera(), 100.0f, 200.0f, 0.0f, slice);
	return ExtrudeTowardLight(slice, 6, L, out, REACH_MAX_PLANES);
}

static void ShadowReachTests()
{
	const float half[3] = { 10, 10, 10 };
	ReachPlane e[REACH_MAX_PLANES];

	// Sun overhead: light travels straight down.
	const float down[3] = { 0, -1, 0 };
	int n = TestReach(down, e);
	Check(n > 0 && n <= REACH_MAX_PLANES, "overhead sun: plane count in range");
	{
		const float above[3] = { 0, 500, -150 };
		Check(!AabbOutsideAnyPlane(e, n, above, half, 0.0f), "box above the slice toward the sun is kept");
		const float beyond[3] = { 0, 0, -300 };
		Check(AabbOutsideAnyPlane(e, n, beyond, half, 0.0f), "box beyond the far plane is cut");
		const float straddle[3] = { 0, 0, -203 };
		Check(!AabbOutsideAnyPlane(e, n, straddle, half, 0.0f), "box straddling the far plane is kept");
		const float below[3] = { 0, -500, -150 };
		Check(AabbOutsideAnyPlane(e, n, below, half, 0.0f), "box under the slice (away from the sun) is cut");
		const float nearMiss[3] = { 0, 0, -215 };
		Check(AabbOutsideAnyPlane(e, n, nearMiss, half, 0.0f), "box 5 past the far plane is cut");
		Check(!AabbOutsideAnyPlane(e, n, nearMiss, half, 6.0f), "growing the box by 6 keeps it");
	}

	// n.L = 0 planes come through unchanged.
	{
		ReachPlane slice[6];
		BuildSlicePlanes(TestCamera(), 100.0f, 200.0f, 0.0f, slice);
		const int zeroIdx[4] = { 0, 1, 4, 5 };   // near, far, right, left
		for (int z = 0; z < 4; ++z)
		{
			bool found = false;
			for (int i = 0; i < n; ++i)
				found = found || SamePlane(e[i], slice[zeroIdx[z]]);
			Check(found, "n.L = 0 plane passes through unchanged");
		}
		Check(n == 6, "overhead sun: 4 unchanged + 1 away + 1 pair");
	}

	// Sun low on the camera's right: light travels left and down.
	const float s = 0.70710678f;
	const float leftDown[3] = { -s, -s, 0 };
	n = TestReach(leftDown, e);
	Check(n > 0, "side sun: planes built");
	{
		const float awayLeft[3] = { -400, 0, -150 };
		Check(AabbOutsideAnyPlane(e, n, awayLeft, half, 0.0f), "box beside the slice away from the sun is cut");
		const float towardSun[3] = { 400, 400, -150 };
		Check(!AabbOutsideAnyPlane(e, n, towardSun, half, 0.0f), "box up and toward the sun is kept");
		const float inside[3] = { 0, 0, -150 };
		Check(!AabbOutsideAnyPlane(e, n, inside, half, 0.0f), "box inside the slice is kept");
	}

	// A general light direction stays within the 12-plane bound.
	{
		const float oblique[3] = { 0.3f, -0.8f, 0.52f };
		Check(TestReach(oblique, e) <= REACH_MAX_PLANES, "oblique light: at most 12 planes");
		ReachPlane tooFew[4];
		ReachPlane slice[6];
		BuildSlicePlanes(TestCamera(), 100.0f, 200.0f, 0.0f, slice);
		Check(ExtrudeTowardLight(slice, 6, oblique, tooFew, 4) == -1, "too small an output array is refused");
	}

	// Shifting along L only grows the region.
	{
		n = TestReach(down, e);
		const float atEdge[3] = { 0, 0, -225 };   // 25 past the far plane, box reaches 215
		Check(AabbOutsideAnyPlane(e, n, atEdge, half, 0.0f), "box past the far plane is cut before the shift");
		const float farDir[3] = { 0, 0, -1 };
		ShiftPlanes(e, n, farDir, 20.0f);
		Check(!AabbOutsideAnyPlane(e, n, atEdge, half, 0.0f), "shifting 20 toward it keeps it");
	}

	// Margins push every slice plane outward.
	{
		ReachPlane slice[6];
		BuildSlicePlanes(TestCamera(), 100.0f, 200.0f, 5.0f, slice);
		const float p[3] = { 0, 0, -204 };
		Check(Dot3(slice[1].n, p) + slice[1].c >= 0.0f, "far plane margin admits depth 204");
		const float q[3] = { 0, 0, -94 };
		Check(Dot3(slice[0].n, q) + slice[0].c < 0.0f, "near plane margin stops short of depth 94");
	}

	{
		const unsigned INF_BITS = 0x7F800000u, NAN_BITS = 0x7FC00000u;
		float inf, nan;
		memcpy(&inf, &INF_BITS, sizeof(inf));
		memcpy(&nan, &NAN_BITS, sizeof(nan));
		const float c[3] = { 1, 2, 3 }, h[3] = { 4, 5, 6 };
		const float hInf[3] = { inf, inf, inf }, cNan[3] = { nan, 0, 0 };
		Check(AabbIsFinite(c, h), "finite box");
		Check(!AabbIsFinite(c, hInf), "infinite half size");
		Check(!AabbIsFinite(cNan, h), "NaN center");
	}

	// The game's far-cascade box at range 1000, fovY 45, 3440x1440.
	{
		float t = tanf(22.5f * 3.14159265f / 180.0f);
		Check(Near(CascadeBoxSide(103.0f, 1000.0f, t, 3440.0f / 1440.0f), 2689.0f, 2.0f), "far box side 2689 at range 1000");
		Check(FrustumDiameter(1000.0f, t, 2.0f) >= 2.0f * 1000.0f * sqrtf(4.0f * t * t + t * t) - 0.5f,
		      "diameter covers the far diagonal");
	}

	{
		const float fwd[3] = { 0, -0.5f, -0.8660254f }, right[3] = { 1, 0, 0 };
		const float el30 = 30.0f * 3.14159265f / 180.0f;
		float el = 0, az = 0;
		const float sunAhead[3] = { 0, -sinf(el30), cosf(el30) };        // L = -sun
		SunElevationAzimuth(sunAhead, fwd, right, &el, &az);
		Check(Near(el, 30.0f, 0.01f) && Near(az, 0.0f, 0.01f), "sun ahead: el 30 az 0");
		const float sunRight[3] = { -cosf(el30), -sinf(el30), 0 };
		SunElevationAzimuth(sunRight, fwd, right, &el, &az);
		Check(Near(az, 90.0f, 0.01f), "sun on the right: az 90");
		const float sunBehind[3] = { 0, -sinf(el30), -cosf(el30) };
		SunElevationAzimuth(sunBehind, fwd, right, &el, &az);
		Check(Near(fabsf(az), 180.0f, 0.01f), "sun behind: az 180");
	}
}

// Test judge for FilterReach: each object is an int, negative = cut,
// 1000+ = infinite, 500..999 = big, anything else kept.
static ReachVerdict JudgeInt(const void* obj, void*)
{
	int v = *(const int*)obj;
	if (v < 0) return RV_CUT;
	if (v >= 1000) return RV_KEEP_INFINITE;
	if (v >= 500) return RV_KEEP_BIG;
	return RV_KEEP;
}

static void ShadowCullTests()
{
	// The sky vector points toward the sun; the light travels the other way.
	{
		const float el30 = 30.0f * 3.14159265f / 180.0f;
		const float towardSun[3] = { 2.0f * cosf(el30), 2.0f * sinf(el30), 0.0f };   // not unit length
		float L[3] = { 0, 0, 0 };
		Check(LightTravelDirection(towardSun, L), "light direction from a sun at elevation 30 in +x");
		Check(L[1] < 0.0f, "light travels down");
		Check(L[0] < 0.0f, "light travels toward -x");
		Check(Near(L[0], -cosf(el30), 1e-5f) && Near(L[1], -sinf(el30), 1e-5f) && L[2] == 0.0f, "L = -normalize(sun)");
		const float zero[3] = { 0, 0, 0 };
		Check(!LightTravelDirection(zero, L), "zero vector refused");
		const unsigned NAN_BITS = 0x7FC00000u;
		float nan;
		memcpy(&nan, &NAN_BITS, sizeof(nan));
		const float bad[3] = { nan, 1, 0 };
		Check(!LightTravelDirection(bad, L), "NaN refused");
	}

	// Composed: a sun straight up, through the light helper, into the reach
	// region. A caster above the slice (toward the sun) can shadow it; one
	// below cannot.
	{
		const float skyUp[3] = { 0, 1, 0 };
		float L[3];
		Check(LightTravelDirection(skyUp, L), "sky straight up gives a light direction");
		ReachPlane slice[6], e[REACH_MAX_PLANES];
		BuildSlicePlanes(TestCamera(), 100.0f, 200.0f, 0.0f, slice);
		int n = ExtrudeTowardLight(slice, 6, L, e, REACH_MAX_PLANES);
		const float half[3] = { 10, 10, 10 };
		const float above[3] = { 0, 500, -150 }, below[3] = { 0, -500, -150 };
		Check(n > 0 && !AabbOutsideAnyPlane(e, n, above, half, 0.0f), "sky up: box above the slice kept");
		Check(n > 0 && AabbOutsideAnyPlane(e, n, below, half, 0.0f), "sky up: box below the slice cut");
	}

	// Ogre's quaternion (w, x, y, z): identity looks down -z; 90 degrees
	// about +y looks down -x.
	{
		const float identity[4] = { 1, 0, 0, 0 };
		float f[3];
		QuaternionForward(identity, f);
		Check(Near(f[0], 0, 1e-6f) && Near(f[1], 0, 1e-6f) && Near(f[2], -1, 1e-6f), "identity forward is -z");
		const float s = 0.70710678f;
		const float yaw90[4] = { s, 0, s, 0 };
		QuaternionForward(yaw90, f);
		Check(Near(f[0], -1, 1e-5f) && Near(f[1], 0, 1e-5f) && Near(f[2], 0, 1e-5f), "90 about +y looks down -x");
	}

	// Nearest depth of a box along the view direction.
	{
		const float pos[3] = { 0, 0, 0 }, fwd[3] = { 0, 0, -1 };
		const float c[3] = { 50, 0, -300 }, h[3] = { 10, 10, 20 };
		Check(Near(AabbMinDepth(pos, fwd, c, h), 280.0f, 1e-4f), "box nearest depth = centre depth - half extent");
		const float behind[3] = { 0, 0, 100 };
		Check(AabbMinDepth(pos, fwd, behind, h) < 0.0f, "box behind the camera has negative depth");
		const float diag[3] = { 0.6f, 0, -0.8f };
		const float c2[3] = { 60, 0, -80 }, h2[3] = { 5, 5, 5 };
		Check(Near(AabbMinDepth(pos, diag, c2, h2), 100.0f - 5.0f * 1.4f, 1e-4f), "oblique view: every axis adds |fwd_k| h_k");
	}

	// Skip rule: a cascade whose receivers all end before dmin.
	{
		const float splits[5] = { 1.0f, 103.0f, 250.0f, 500.0f, 1000.0f };
		Check(CascadeSkipMask(splits, 4, 1165.0f, 10.0f) == 0xFu, "nothing in range: all four skipped");
		Check(CascadeSkipMask(splits, 4, 300.0f, 10.0f) == 0x3u, "dmin 300: cascades 0 and 1");
		Check(CascadeSkipMask(splits, 4, 255.0f, 10.0f) == 0x1u, "cascade 1 ends at 260 with the margin: kept");
		Check(CascadeSkipMask(splits, 4, 260.0f, 10.0f) == 0x1u, "a receiver exactly at split + margin keeps its cascade");
		Check(CascadeSkipMask(splits, 4, 5.0f, 10.0f) == 0u, "dmin before the first split: nothing skipped");
		Check(CascadeSkipMask(splits, 4, -50.0f, 10.0f) == 0u, "negative dmin: nothing skipped");
		const unsigned NAN_BITS = 0x7FC00000u, INF_BITS = 0x7F800000u;
		float nan, inf;
		memcpy(&nan, &NAN_BITS, sizeof(nan));
		memcpy(&inf, &INF_BITS, sizeof(inf));
		Check(CascadeSkipMask(splits, 4, nan, 10.0f) == 0u, "NaN dmin: nothing skipped");
		Check(CascadeSkipMask(splits, 4, inf, 10.0f) == 0u, "infinite dmin (no receiver seen): nothing skipped");
		Check(CascadeSkipMask(splits, 0, 1165.0f, 10.0f) == 0u && CascadeSkipMask(splits, 9, 1165.0f, 10.0f) == 0u,
		      "cascade count outside 1..8: nothing skipped");
		Check(CascadeSkipMask(NULL, 4, 1165.0f, 10.0f) == 0u, "no splits: nothing skipped");
	}

	// Order keys: integer order matches float order, and they round-trip.
	{
		const float v[7] = { -1e9f, -5.0f, -1.0f, 0.0f, 1.0f, 280.5f, 1e9f };
		bool ordered = true, round = true;
		for (int i = 0; i < 7; ++i)
		{
			if (i > 0 && !(FloatOrderKey(v[i - 1]) < FloatOrderKey(v[i])))
				ordered = false;
			if (FloatFromOrderKey(FloatOrderKey(v[i])) != v[i])
				round = false;
		}
		Check(ordered, "order keys sort like the floats");
		Check(round, "order keys round-trip");
		Check(FloatOrderKey(-0.0f) <= FloatOrderKey(0.0f), "-0 does not sort above +0");
	}

	// FilterReach: counts, compaction in order, NULLs kept and not counted.
	{
		int vals[7] = { 5, -1, 700, 1200, -3, 42, 0 };
		void* data[9];
		data[0] = (void*)0x1;   // before the range: untouched
		for (int i = 0; i < 7; ++i)
			data[1 + i] = &vals[i];
		data[8] = NULL;
		void* copy[9];
		memcpy(copy, data, sizeof(data));

		ReachTally t;
		size_t end = FilterReach(data, 1, 9, false, &JudgeInt, NULL, &t);
		Check(end == 9 && memcmp(copy, data, sizeof(data)) == 0, "count only: nothing moves");
		Check(t.in == 7 && t.reach == 5 && t.cut == 2 && t.inf == 1 && t.big == 1, "tally: in/reach/cut/inf/big");

		end = FilterReach(data, 1, 9, true, &JudgeInt, NULL, &t);
		Check(end == 7, "compact: two cut, the rest kept");
		Check(data[0] == (void*)0x1, "entries before the range untouched");
		Check(data[1] == &vals[0] && data[2] == &vals[2] && data[3] == &vals[3] && data[4] == &vals[5] &&
		      data[5] == &vals[6] && data[6] == NULL, "kept entries keep their order, NULL kept");
		Check(t.in == 7 && t.cut == 2, "compaction counts the same");

		end = FilterReach(data, 3, 3, true, &JudgeInt, NULL, &t);
		Check(end == 3 && t.in == 0, "empty range");
	}
}

static std::string GameRoot()
{
	char buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("KENSHI_ROOT", buf, sizeof(buf));
	if (n > 0 && n < sizeof(buf))
		return std::string(buf, n);
	return "G:\\SteamLibrary\\steamapps\\common\\Kenshi";
}

typedef void (*OgreMurmur_t)(const void* key, int len, unsigned int seed, void* out);

static void IdStringTests()
{
	// Published MurmurHash3_x86_32 vectors.
	Check(MurmurHash3_x86_32("", 0, 0) == 0, "murmur: empty, seed 0");
	Check(MurmurHash3_x86_32("", 0, 1) == 0x514E28B7, "murmur: empty, seed 1");
	Check(MurmurHash3_x86_32("hello", 5, 0) == 0x248BFA47, "murmur: hello");
	Check(MurmurHash3_x86_32("The quick brown fox jumps over the lazy dog", 43, 0x9747B28C) == 0x2FA826CD,
	      "murmur: quick brown fox");

	// The game hashes compositor names at runtime through OgreMain's own export
	// (no hash immediate in the exe): compare against it for every name used.
	const char* names[] = { "Debug", "InteriorMask", "Water_Reflection", "Kenshi_Main", "abc", "abcd", "" };
	std::string path = GameRoot() + "\\OgreMain_x64.dll";
	HMODULE ogre = LoadLibraryExA(path.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
	Check(ogre != NULL, "load OgreMain for the IdString check");
	if (ogre)
	{
		OgreMurmur_t mm = (OgreMurmur_t)GetProcAddress(ogre, "?MurmurHash3_x86_32@@YAXPEBXHIPEAX@Z");
		Check(mm != NULL, "OgreMain exports MurmurHash3_x86_32");
		for (size_t i = 0; mm && i < sizeof(names) / sizeof(names[0]); ++i)
		{
			unsigned int theirs = 0;
			mm(names[i], (int)strlen(names[i]), 0x3A8EFA67, &theirs);
			if (IdStringHash(names[i]) != theirs)
				printf("  IdString(\"%s\") ours %08X OgreMain %08X\n", names[i], IdStringHash(names[i]), theirs);
			Check(IdStringHash(names[i]) == theirs, "IdStringHash matches OgreMain's MurmurHash3 export");
		}
		FreeLibrary(ogre);
	}
	Check(IdStringHash("Debug") == 0x3D95EEEE, "IdString(Debug)");
	Check(IdStringHash("InteriorMask") == 0xD29CE51F, "IdString(InteriorMask)");
	Check(IdStringHash("Kenshi_Main") == 0xD5EF3D65, "IdString(Kenshi_Main)");
}

static void CompositorLayoutTests()
{
	std::string root = GameRoot();
	HMODULE ogre = LoadLibraryExA((root + "\\OgreMain_x64.dll").c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
	HMODULE d3d = LoadLibraryExA((root + "\\RenderSystem_Direct3D11_x64.dll").c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
	Check(ogre != NULL, "load OgreMain for the compositor layout");
	if (ogre)
	{
		Check(VerifyCompositorLayout(ogre), "compositor layout verifies against the shipped OgreMain");
		const char* syms[] = { SYM_WS_FIND_NODE, SYM_NODE_SET_ENABLED, SYM_NODE_GET_ENABLED, SYM_WS_SET_LISTENER,
		                       SYM_WS_GET_LISTENER, SYM_WS_DEFINITION_NAME, SYM_WS_SCENE_MANAGER };
		for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); ++i)
			Check(GetProcAddress(ogre, syms[i]) != NULL, "every compositor export the lever calls resolves");
		// setEnabled/getEnabled only write and read CompositorNode+10h.
		const unsigned char set[4] = { 0x88,0x51,0x10,0xC3 };
		const unsigned char get[5] = { 0x0F,0xB6,0x41,0x10,0xC3 };
		Check(memcmp(GetProcAddress(ogre, SYM_NODE_SET_ENABLED), set, sizeof(set)) == 0, "setEnabled is a byte store");
		Check(memcmp(GetProcAddress(ogre, SYM_NODE_GET_ENABLED), get, sizeof(get)) == 0, "getEnabled is a byte load");
	}
	Check(!VerifyCompositorLayout(d3d), "compositor layout refuses the wrong module");
	Check(!VerifyCompositorLayout(NULL), "compositor layout refuses a missing module");
	if (ogre)
		Check(VerifyPassVisibilityLayout(ogre), "pass visibility layout verifies against the shipped OgreMain");
	Check(!VerifyPassVisibilityLayout(d3d), "pass visibility layout refuses the wrong module");
	Check(!VerifyPassVisibilityLayout(NULL), "pass visibility layout refuses a missing module");
	if (ogre) FreeLibrary(ogre);
	if (d3d) FreeLibrary(d3d);
}

// The planes of the cube [-10, 10]^3, normals pointing in, as Ogre stores a
// frustum: an object is inside a plane when n.p + d > 0.
static void CubePlanes(FrustumPlane* p, float offsetX)
{
	const float n[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
	for (int i = 0; i < 6; ++i)
	{
		p[i].nx = n[i][0];
		p[i].ny = n[i][1];
		p[i].nz = n[i][2];
		p[i].d = 10.0f + (i == 0 ? -offsetX : i == 1 ? offsetX : 0.0f);
	}
}

static unsigned int FloatBits(float f) { unsigned int u; memcpy(&u, &f, 4); return u; }
static float BitsFloat(unsigned int u) { float f; memcpy(&f, &u, 4); return f; }

// MovableObject::cullFrustum's plane and infinity test for four objects, as
// the shipped OgreMain computes it (flags, distance and mask left out).
static int OgreCullLanes(const FrustumPlane* planes, const float* c[3], const float* h[3])
{
	const __m128 signMask = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000));
	const __m128 one = _mm_set1_ps(1.0f);
	const __m128 inf = _mm_set1_ps(BitsFloat(0x7F800000u));
	__m128 cx = _mm_loadu_ps(c[0]), cy = _mm_loadu_ps(c[1]), cz = _mm_loadu_ps(c[2]);
	__m128 hx = _mm_loadu_ps(h[0]), hy = _mm_loadu_ps(h[1]), hz = _mm_loadu_ps(h[2]);
	__m128 inside = _mm_castsi128_ps(_mm_set1_epi32(-1));
	for (int i = 0; i < 6; ++i)
	{
		__m128 nx = _mm_set1_ps(planes[i].nx), ny = _mm_set1_ps(planes[i].ny), nz = _mm_set1_ps(planes[i].nz);
		__m128 sx = _mm_or_ps(_mm_and_ps(nx, signMask), one);
		__m128 sy = _mm_or_ps(_mm_and_ps(ny, signMask), one);
		__m128 sz = _mm_or_ps(_mm_and_ps(nz, signMask), one);
		__m128 negD = _mm_xor_ps(_mm_set1_ps(planes[i].d), signMask);
		__m128 x = _mm_mul_ps(nx, _mm_add_ps(cx, _mm_mul_ps(hx, sx)));
		__m128 y = _mm_mul_ps(ny, _mm_add_ps(cy, _mm_mul_ps(hy, sy)));
		__m128 z = _mm_mul_ps(nz, _mm_add_ps(cz, _mm_mul_ps(hz, sz)));
		inside = _mm_and_ps(inside, _mm_cmplt_ps(negD, _mm_add_ps(z, _mm_add_ps(x, y))));
	}
	__m128 infinite = _mm_or_ps(_mm_or_ps(_mm_cmpeq_ps(hy, inf), _mm_cmpeq_ps(hx, inf)), _mm_cmpeq_ps(hz, inf));
	return _mm_movemask_ps(_mm_or_ps(inside, infinite));
}

static unsigned int s_rng = 12345;
static unsigned int NextRand() { s_rng = s_rng * 1103515245u + 12345u; return s_rng >> 8; }
static float RandRange(float lo, float hi) { return lo + (hi - lo) * (float)(NextRand() & 0xFFFF) / 65535.0f; }

static float RandCoord()
{
	switch (NextRand() % 16)
	{
	case 0:  return BitsFloat(0x7FC00000u);    // NaN
	case 1:  return 0.0f;
	case 2:  return -0.0f;
	default: return RandRange(-30.0f, 30.0f);
	}
}

static float RandHalf()
{
	switch (NextRand() % 12)
	{
	case 0:  return BitsFloat(0x7F800000u);    // +inf: infinite box
	case 1:  return BitsFloat(0xFF800000u);    // -inf: null box
	case 2:  return 0.0f;
	default: return RandRange(0.0f, 8.0f);
	}
}

static void RandPlanes(FrustumPlane* p)
{
	for (int i = 0; i < 6; ++i)
	{
		float v[3];
		for (int k = 0; k < 3; ++k)
			v[k] = (NextRand() % 5 == 0) ? ((NextRand() & 1) ? 0.0f : -0.0f) : RandRange(-1.0f, 1.0f);
		float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (len > 0.0f)
			for (int k = 0; k < 3; ++k)
				v[k] /= len;
		p[i].nx = v[0];
		p[i].ny = v[1];
		p[i].nz = v[2];
		p[i].d = RandRange(-20.0f, 40.0f);
	}
}

static void PassVisibilityTests()
{
	const unsigned int SHOWN = 0x800 | LAYER_VISIBILITY;
	FrustumPlane cube[6];
	CubePlanes(cube, 0.0f);
	const float origin[3] = { 0, 0, 0 };
	const float unitHalf[3] = { 1, 1, 1 };
	const float farOut[3] = { 50, 0, 0 };
	const float edge[3] = { 10.5f, 0, 0 };
	const float nearOut[3] = { 11.5f, 0, 0 };   // 0.5 outside the x = 10 plane
	Check(ObjectMayDraw(SHOWN, origin, unitHalf, cube, false), "a box inside the frustum may draw");
	Check(!ObjectMayDraw(SHOWN, farOut, unitHalf, cube, true), "a box outside the frustum does not");
	Check(ObjectMayDraw(SHOWN, edge, unitHalf, cube, false), "a box across a plane may draw");
	Check(!ObjectMayDraw(SHOWN, nearOut, unitHalf, cube, false), "exactly as Ogre: just outside is culled");
	Check(ObjectMayDraw(SHOWN, nearOut, unitHalf, cube, true), "loosened: just outside is kept");
	Check(!ObjectMayDraw(0, origin, unitHalf, cube, true), "no flags: culled");
	Check(!ObjectMayDraw(0x800, origin, unitHalf, cube, true), "hidden (no LAYER_VISIBILITY): culled");
	Check(!ObjectMayDraw(0x20000000 | LAYER_VISIBILITY, origin, unitHalf, cube, true), "flags above the pass mask bits: culled");
	Check(!ObjectMayDraw(0x80000000u | LAYER_VISIBILITY, origin, unitHalf, cube, true), "shadow-caster bit alone: culled");
	const float infHalf[3] = { BitsFloat(0x7F800000u), 1, 1 };
	Check(ObjectMayDraw(SHOWN, farOut, infHalf, cube, false), "an infinite box always may draw");
	Check(!ObjectMayDraw(0x800, farOut, infHalf, cube, false), "an infinite box still needs its flags");
	const float nullHalf[3] = { BitsFloat(0xFF800000u), BitsFloat(0xFF800000u), BitsFloat(0xFF800000u) };
	Check(!ObjectMayDraw(SHOWN, origin, nullHalf, cube, true), "a null box (empty object) is culled");
	const float nanCentre[3] = { BitsFloat(0x7FC00000u), 0, 0 };
	Check(!ObjectMayDraw(SHOWN, nanCentre, unitHalf, cube, true), "a NaN box is culled, as Ogre culls it");

	// Against the shipped cull's arithmetic, special values included: exact
	// without the margin, never stricter with it.
	int mismatches = 0, stricter = 0;
	for (int round = 0; round < 20000; ++round)
	{
		FrustumPlane planes[6];
		RandPlanes(planes);
		float cx[4], cy[4], cz[4], hx[4], hy[4], hz[4];
		for (int j = 0; j < 4; ++j)
		{
			cx[j] = RandCoord(); cy[j] = RandCoord(); cz[j] = RandCoord();
			hx[j] = RandHalf(); hy[j] = RandHalf(); hz[j] = RandHalf();
		}
		const float* c[3] = { cx, cy, cz };
		const float* h[3] = { hx, hy, hz };
		int ogre = OgreCullLanes(planes, c, h);
		for (int j = 0; j < 4; ++j)
		{
			const float centre[3] = { cx[j], cy[j], cz[j] };
			const float half[3] = { hx[j], hy[j], hz[j] };
			bool kept = (ogre >> j) & 1;
			if (ObjectMayDraw(SHOWN, centre, half, planes, false) != kept)
				++mismatches;
			if (kept && !ObjectMayDraw(SHOWN, centre, half, planes, true))
				++stricter;
		}
	}
	Check(mismatches == 0, "the plane test matches OgreMain's cull lane for lane");
	Check(stricter == 0, "the loosened test keeps every object OgreMain keeps");
	Check(FloatBits(-0.0f) == 0x80000000u, "test float helpers");

	// One queue slot: a pool vector of ten arrays, five objects in two blocks.
	static float aabb[2 * AABB_BLOCK_FLOATS];
	static unsigned int flags[8];
	static const void* pools[10];
	static char slot[QUEUE_SLOT_SIZE];
	for (int k = 0; k < 8; ++k)
	{
		float* block = aabb + (k / 4) * AABB_BLOCK_FLOATS;
		block[k % 4] = 50.0f;                      // centre x: outside
		block[4 + k % 4] = block[8 + k % 4] = 0.0f;
		block[12 + k % 4] = block[16 + k % 4] = block[20 + k % 4] = 1.0f;
		flags[k] = SHOWN;
	}
	for (int i = 0; i < 10; ++i)
		pools[i] = NULL;
	pools[POOL_WORLD_AABB] = aabb;
	pools[POOL_VIS_FLAGS] = flags;
	*(const void***)(slot + QUEUE_POOLS_BEGIN) = pools;
	*(const void***)(slot + QUEUE_POOLS_END) = pools + 10;
	*(size_t*)(slot + QUEUE_USED) = 5;
	QueueObjects q;
	Check(QueueSlotObjects(slot, &q) && q.count == 5 && q.worldAabb == aabb && q.visFlags == flags,
	      "queue slot: object count and the AABB and flag pools");
	const FrustumPlane* one[1] = { cube };
	size_t budget = 100;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_HIDDEN && budget == 92,
	      "all outside: hidden, whole blocks tested");
	aabb[AABB_BLOCK_FLOATS + 2] = 0.0f;           // object 6: a padding lane of the last block
	budget = 100;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_MAY_DRAW, "the last block's padding lanes are tested, as cull tests them");
	aabb[AABB_BLOCK_FLOATS + 2] = 50.0f;
	aabb[AABB_BLOCK_FLOATS + 0] = 0.0f;           // object 4 inside
	budget = 100;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_MAY_DRAW, "one object inside: may draw");
	flags[4] = 0x800;
	budget = 100;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_HIDDEN, "that object hidden: hidden");
	flags[4] = SHOWN;
	budget = 3;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_OVER_BUDGET && budget == 0, "over the budget: gives up");
	aabb[AABB_BLOCK_FLOATS + 0] = 50.0f;
	FrustumPlane shifted[6];
	CubePlanes(shifted, 45.0f);                   // [35, 55] on x
	const FrustumPlane* two[2] = { cube, shifted };
	budget = 100;
	Check(ScanQueueObjects(q, two, 2, &budget) == SCAN_MAY_DRAW, "seen by the second camera only: may draw");
	*(const void***)(slot + QUEUE_POOLS_END) = pools + 7;
	Check(!QueueSlotObjects(slot, &q), "a pool vector without the flag pool is refused");
	*(size_t*)(slot + QUEUE_USED) = 0;
	Check(QueueSlotObjects(slot, &q) && q.count == 0, "an unused queue needs no pools");
	budget = 100;
	Check(ScanQueueObjects(q, one, 1, &budget) == SCAN_HIDDEN && budget == 100, "an unused queue tests nothing");
}

// A fake per-queue slot array, laid out as ObjectMemoryManager's.
struct FakeQueues
{
	char   slots[QUEUE_SLOT_SIZE * 20];
	size_t freed[20][4];
	void Set(size_t rq, size_t used, size_t freedCount)
	{
		char* q = slots + rq * QUEUE_SLOT_SIZE;
		*(size_t*)(q + QUEUE_USED) = used;
		*(size_t**)(q + QUEUE_FREE_BEGIN) = freed[rq];
		*(size_t**)(q + QUEUE_FREE_END) = freed[rq] + freedCount;
	}
};

static FakeQueues g_fakeQueues;   // static storage: starts zeroed

static void EmptyPassTests()
{
	FakeQueues& f = g_fakeQueues;
	const char* begin = f.slots;
	const char* end17 = f.slots + 17 * QUEUE_SLOT_SIZE;   // queues 0..16 exist
	f.Set(16, 3, 1);
	f.Set(5, 2, 2);
	Check(LiveObjectsInQueue(begin, end17, 16) == 2, "live = used less freed slots");
	Check(LiveObjectsInQueue(begin, end17, 5) == 0, "all slots freed -> empty");
	Check(LiveObjectsInQueue(begin, end17, 3) == 0, "never used -> empty");
	Check(LiveObjectsInQueue(begin, end17, 17) == 0, "queue past the array -> empty, not read");
	Check(LiveObjectsInQueue(begin, end17, 87) == 0, "far queue past the array -> empty");
	Check(LiveObjectsInQueue(begin, begin, 0) == 0, "no queues -> empty");
	Check(LiveObjectsInQueue(NULL, NULL, 0) == 0, "NULL array -> empty");
	f.Set(16, 1, 3);
	Check(LiveObjectsInQueue(begin, end17, 16) == 0, "more freed than used never underflows");

	// The mask node: off only after a frame run empty.
	bool ran = false;
	Check(MaskNodeWanted(true, false, true, &ran) && !ran, "occupied: runs");
	Check(MaskNodeWanted(false, false, true, &ran) && ran, "first empty frame: runs once to clear");
	Check(!MaskNodeWanted(false, false, true, &ran) && ran, "second empty frame: off");
	Check(!MaskNodeWanted(false, false, false, &ran) && ran, "stays off while empty");
	Check(MaskNodeWanted(false, true, false, &ran) && ran, "resize while off: runs to clear the new texture");
	Check(!MaskNodeWanted(false, false, true, &ran), "after the resize frame: off again");
	Check(MaskNodeWanted(true, false, false, &ran) && !ran, "an object appears: runs the same frame");
	Check(MaskNodeWanted(false, false, true, &ran) && ran, "object gone: one clearing frame first");
	ran = true;
	Check(MaskNodeWanted(true, true, false, &ran) && !ran, "occupied wins over resize");

	const RenderKey* key = FindRenderKey("emptyPassSkip");
	Check(key && key->kind == RK_BOOL && key->live && key->label && key->devOnly,
	      "emptyPassSkip is a live bool with a DEV-only row");
	Check(RenderConfigDefaults().emptyPassSkip, "emptyPassSkip defaults on");
	RenderConfig c = RenderConfigDefaults();
	Check(ParseRenderKey(&c, "emptyPassSkip", "false") == RP_OK && !c.emptyPassSkip, "emptyPassSkip parses");
}

static const char* GET_CONSTANT_DEFINITION =
	"?getConstantDefinition@GpuProgramParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z";

static RenderConfig SampleRenderConfig()
{
	RenderConfig c;
	memset(&c, 0, sizeof(c));
	c.renderLevers = true;
	c.reflectionHalfRate = true;
	c.particleOffscreenSkip = true;
	c.renderDiag = true;
	c.particleStepCapSpeed = 3.0f;
	c.particleOffscreenSeconds = 0.5f;
	c.particleOffscreenMinAge = 10.0f;
	c.foliageBudgetSpeed = 3.0f;
	strcpy_s(c.particleLoopingNames, sizeof(c.particleLoopingNames), "fire,smoke");
	return c;
}

// Applies every key=value line of text over base, the last one winning, as
// LoadConfig does over the compiled-in defaults.
static RenderConfig ParseRenderText(const std::string& text, const RenderConfig& base)
{
	RenderConfig c = base;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? text.size() : nl + 1;
		std::string key, val;
		if (SplitIniLine(line, &key, &val))
			ParseRenderKey(&c, key, val);
	}
	return c;
}

static bool AllKeysEqual(const RenderConfig& a, const RenderConfig& b)
{
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		if (!RenderValueEqual(a, b, g_renderKeys[i]))
			return false;
	}
	return true;
}

static void CheckText(const std::string& got, const std::string& expected, const char* what)
{
	Check(got == expected, what);
	if (got != expected)
		printf("---- got ----\n%s\n---- expected ----\n%s\n", got.c_str(), expected.c_str());
}

static IniEntry Entry(const char* key, const char* value, IniValueKind kind, bool append)
{
	IniEntry e;
	e.key = key;
	e.value = value;
	e.kind = kind;
	e.append = append;
	return e;
}

static void RenderKeysTests()
{
	// Parse.
	{
		RenderConfig c = SampleRenderConfig();
		Check(ParseRenderKey(&c, "particleStepCap", "on") == RP_OK && c.particleStepCap, "bool key parses");
		Check(ParseRenderKey(&c, "particleStepCap", "maybe") == RP_BAD_VALUE && c.particleStepCap, "bad bool leaves the field");
		Check(ParseRenderKey(&c, "particleStepCapSpeed", "abc") == RP_BAD_VALUE && c.particleStepCapSpeed == 3.0f,
		      "bad float leaves the field");
		Check(ParseRenderKey(&c, "particleOffscreenSeconds", "0.25") == RP_OK && c.particleOffscreenSeconds == 0.25f,
		      "float key parses");
		Check(ParseRenderKey(&c, "deferral", "true") == RP_NOT_RENDER, "other keys are not render keys");
		Check(ParseRenderKey(&c, "particleLoopingNames", "torch") == RP_OK &&
		      strcmp(c.particleLoopingNames, "torch") == 0, "text key parses");
		std::string longText(400, 'x');
		ParseRenderKey(&c, "particleLoopingNames", longText);
		Check(strlen(c.particleLoopingNames) == sizeof(c.particleLoopingNames) - 1, "text key is truncated to its buffer");
	}

	// Value text reads back to the same float.
	{
		RenderConfig c = SampleRenderConfig();
		c.particleOffscreenSeconds = 0.1f;
		c.particleStepCapSpeed = 1.0f / 3.0f;
		const RenderKey* secs = FindRenderKey("particleOffscreenSeconds");
		const RenderKey* speed = FindRenderKey("particleStepCapSpeed");
		Check(secs && FormatRenderValue(c, *secs) == "0.1", "short float keeps six digits");
		RenderConfig back = c;
		back.particleStepCapSpeed = 0.0f;
		Check(speed && ParseRenderKey(&back, "particleStepCapSpeed", FormatRenderValue(c, *speed)) == RP_OK &&
		      back.particleStepCapSpeed == c.particleStepCapSpeed, "float text reads back to the same bits");
	}

	// Clamp.
	{
		RenderConfig fallback = SampleRenderConfig();
		RenderConfig c = fallback;
		c.particleStepCapSpeed = 50.0f;
		c.particleOffscreenMinAge = -1.0f;
		std::vector<std::string> notes;
		ClampRenderValues(&c, fallback, &notes);
		Check(c.particleStepCapSpeed == 20.0f && c.particleOffscreenMinAge == 0.0f, "clamp stores the bound");
		Check(notes.size() == 2 && notes[0] == "particleStepCapSpeed=50 clamped to max 20" &&
		      notes[1] == "particleOffscreenMinAge=-1 clamped to min 0", "clamp notes name key, value and bound");

		const unsigned INF_BITS = 0x7F800000u, NAN_BITS = 0x7FC00000u;
		float inf, nan;
		memcpy(&inf, &INF_BITS, sizeof(inf));
		memcpy(&nan, &NAN_BITS, sizeof(nan));
		c = fallback;
		c.particleStepCapSpeed = nan;
		c.particleOffscreenSeconds = inf;
		c.particleOffscreenMinAge = -inf;
		notes.clear();
		ClampRenderValues(&c, fallback, &notes);
		Check(c.particleStepCapSpeed == 3.0f && c.particleOffscreenSeconds == 0.5f && c.particleOffscreenMinAge == 10.0f,
		      "non-finite floats take the fallback value");
		Check(notes.size() == 3 && notes[0].find("particleStepCapSpeed=") == 0 &&
		      notes[0].find("is not a finite number, kept 3") != std::string::npos, "non-finite note names the fallback");
	}

	// Diff.
	{
		RenderConfig live = SampleRenderConfig();
		RenderConfig next = live;
		std::vector<RenderChange> changes;
		DiffRenderConfig(live, next, &changes);
		Check(changes.empty(), "identical configs have no changes");

		next.particleStepCap = true;
		next.particleStepCapSpeed = 5.0f;
		strcpy_s(next.particleLoopingNames, sizeof(next.particleLoopingNames), "fire");
		DiffRenderConfig(live, next, &changes);
		Check(changes.size() == 3, "diff lists exactly the changed keys");
		if (changes.size() == 3)
		{
			Check(strcmp(changes[0].key->name, "particleStepCap") == 0 &&
			      changes[0].before == "false" && changes[0].after == "true", "bool change, old and new");
			Check(strcmp(changes[1].key->name, "particleStepCapSpeed") == 0 &&
			      changes[1].before == "3" && changes[1].after == "5", "float change, old and new");
			Check(strcmp(changes[2].key->name, "particleLoopingNames") == 0 &&
			      changes[2].before == "fire,smoke" && changes[2].after == "fire", "text change, old and new");
			Check(changes[0].key->live && changes[2].key->live, "lever keys apply at runtime");
		}

		changes.clear();
		next = live;
		next.renderLevers = false;
		DiffRenderConfig(live, next, &changes);
		Check(changes.size() == 1 && !changes[0].key->live, "renderLevers is listed but startup-only");

		RenderConfig copy = live;
		CopyRenderValue(&copy, next, *FindRenderKey("renderLevers"));
		Check(!copy.renderLevers && copy.reflectionHalfRate, "copy writes one field only");
	}

	// Generic rewrite on a key that is not a render key.
	{
		std::vector<IniEntry> e(1, Entry("navmeshWorkerCount", "4", INI_INT, true));
		CheckText(RewriteIniKeys("navmeshWorkerCount = 04\r\n", e, NULL), "navmeshWorkerCount = 04\r\n",
		          "int line with an equal value is untouched");
		CheckText(RewriteIniKeys("a=1\r\nnavmeshWorkerCount = 3 \r\nb=2", e, NULL), "a=1\r\nnavmeshWorkerCount = 4 \r\nb=2",
		          "int line gets the new value in place");
		CheckText(RewriteIniKeys("[Render]\r\nx=1\r\n\r\n[Other]\r\n", e, NULL),
		          "navmeshWorkerCount=4\r\n[Render]\r\nx=1\r\n\r\n[Other]\r\n", "no section named: added before the first section");
		CheckText(RewriteIniKeys("; w\r\n# navmeshWorkerCount=0\r\n\r\n[Render]\r\nx=1\r\n", e, NULL),
		          "; w\r\n# navmeshWorkerCount=0\r\nnavmeshWorkerCount=4\r\n\r\n[Render]\r\nx=1\r\n",
		          "no section named: after the last non-blank line above the first section");
		CheckText(RewriteIniKeys("a=1\r\nb=2", e, NULL), "a=1\r\nb=2\r\nnavmeshWorkerCount=4\r\n",
		          "no section named, no section in the text: appended at the end");
		e[0].append = false;
		CheckText(RewriteIniKeys("a=1\r\n", e, NULL), "a=1\r\n", "an entry not marked append is not added");
		CheckText(RewriteIniKeys("# navmeshWorkerCount=3\r\n", e, NULL), "# navmeshWorkerCount=3\r\n",
		          "a key inside a comment line is not a line for that key");
	}

	// INI rewrite: render lines change in place, everything else is kept; only
	// keys that differ from the defaults are added.
	{
		RenderConfig defaults = SampleRenderConfig();
		RenderConfig cfg = defaults;
		cfg.particleStepCap = true;
		cfg.particleStepCapSpeed = 5.0f;
		cfg.renderDiag = false;
		cfg.particleOffscreenMinAge = 20.0f;
		const std::string text =
			"# KEO settings\r\n"
			"# particleStepCap=false\r\n"
			"deferral=true\r\n"
			"  particleStepCap = off   \r\n"
			"[Render]\r\n"
			"reflectionHalfRate=1\r\n"
			"; a comment\r\n"
			"particleStepCapSpeed=3\r\n"
			"particleStepCap=off\r\n"
			"particleOffscreenSeconds=\r\n"
			"\r\n"
			"[Other]\r\n"
			"preload=true";
		const std::string expected =
			"# KEO settings\r\n"
			"# particleStepCap=false\r\n"
			"deferral=true\r\n"
			"  particleStepCap = true   \r\n"
			"[Render]\r\n"
			"reflectionHalfRate=1\r\n"
			"; a comment\r\n"
			"particleStepCapSpeed=5\r\n"
			"particleStepCap=true\r\n"
			"particleOffscreenSeconds=\r\n"
			"renderDiag=false\r\n"
			"particleOffscreenMinAge=20\r\n"
			"\r\n"
			"[Other]\r\n"
			"preload=true";
		std::string out = RewriteRenderIni(text, cfg, defaults);
		CheckText(out, expected, "rewrite: in place, comments and duplicates, non-default keys at the end of [Render]");
		Check(AllKeysEqual(ParseRenderText(out, defaults), cfg), "rewritten text parses back to the config");
		CheckText(RewriteRenderIni(out, cfg, defaults), out, "rewrite of its own output changes nothing");
	}

	// The loader has no inline comments: the value is everything after '='.
	// A trailing comment on a bool makes the line unreadable to it (so it is
	// rewritten and the comment goes); strtod stops at the comment on a float,
	// so an equal float keeps its line and a changed one loses the comment.
	{
		RenderConfig defaults = SampleRenderConfig();
		RenderConfig cfg = defaults;
		cfg.particleOffscreenMinAge = 20.0f;
		const std::string text =
			"reflectionHalfRate=true # keep\r\n"
			"particleStepCapSpeed=3 ; three\r\n"
			"particleOffscreenMinAge=10 ; minimum\r\n";
		const std::string expected =
			"reflectionHalfRate=true\r\n"
			"particleStepCapSpeed=3 ; three\r\n"
			"particleOffscreenMinAge=20\r\n";
		CheckText(RewriteRenderIni(text, cfg, defaults), expected, "inline trailing comments, as the loader reads them");
	}

	// Mixed endings: each line keeps its own; added lines take the first one's.
	{
		RenderConfig defaults = SampleRenderConfig();
		RenderConfig cfg = defaults;
		cfg.particleStepCap = true;
		cfg.renderDiag = false;
		CheckText(RewriteRenderIni("a=1\r\nparticleStepCap=false\nb=2\r\n", cfg, defaults),
		          "a=1\r\nparticleStepCap=true\nb=2\r\nrenderDiag=false\r\n", "mixed CRLF/LF text");
	}

	// Additions only for keys that differ from the defaults.
	{
		RenderConfig defaults = SampleRenderConfig();
		CheckText(RewriteRenderIni("", defaults, defaults), "", "nothing to add when every key is at its default");
		CheckText(RewriteRenderIni("# one comment", defaults, defaults), "# one comment", "text is untouched when nothing changes");
		RenderConfig cfg = defaults;
		cfg.renderLevers = false;
		CheckText(RewriteRenderIni("", cfg, defaults), "renderLevers=false\r\n", "empty text gets the changed key, CRLF");
		CheckText(RewriteRenderIni("# one comment", cfg, defaults), "# one comment\r\nrenderLevers=false\r\n",
		          "a last line without an ending gets one");
		CheckText(RewriteRenderIni("a=1\nrenderLevers=true\n", defaults, defaults), "a=1\nrenderLevers=true\n",
		          "an existing line at the default value is kept");
		CheckText(RewriteRenderIni("a=1\nrenderLevers=true\n", cfg, defaults), "a=1\nrenderLevers=false\n",
		          "an existing line is updated whatever the default");
		CheckText(RewriteRenderIni("a=1\n", cfg, defaults), "a=1\nrenderLevers=false\n", "LF text, no section: LF");
	}
}

// The KEO.ini template at repo root, which build_tests.bat cd's
// into before running this binary: every line must be inert (a comment, a
// section header or blank) so LoadConfig's parse path leaves the compiled
// defaults untouched, and every documented "# key=value" default must
// actually match the compiled one.
static std::string FirstEol(const std::string& text)
{
	size_t nl = text.find('\n');
	if (nl == std::string::npos)
		return "\r\n";
	return (nl > 0 && text[nl - 1] == '\r') ? "\r\n" : "\n";
}

// A map node as VS2010 lays out std::map<std::string, T>'s value: the key
// string, then the mapped definition right after it.
struct FakeDef { int type; size_t physicalIndex; };
struct FakeNode { std::string key; FakeDef def; };

static std::string ConstName(int i)
{
	char buf[64];
	// Every third name is longer than 15 characters, so its text is on the heap.
	sprintf_s(buf, sizeof(buf), i % 3 == 0 ? "lightSpecularColourPowerScaled%d" : "c%d", i);
	return buf;
}

// Hits at the fixed addresses below, measured once; a change means the hash
// or the probe rule changed. At 2,000 keys (half full) three keys still
// lose their probe window to eviction.
static const int HITS_5000 = 4032;
static const int HITS_2000 = 1997;

// Stand-in buffer and program-node addresses: never dereferenced.
static const void* FakePtr(size_t n) { return (const void*)(0x10000 + n * 0x40); }

static void UploadShadowTests()
{
	UploadShadowTable t;
	memset(&t, 0, sizeof(t));
	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == NULL, "unconfigured table refuses");
	t.Configure(8, 64);
	Check(t.slots == NULL, "no slot array before the first acquire");

	UploadShadow* a = t.Acquire(FakePtr(1), FakePtr(100), 16);
	Check(a && !a->valid && a->size == 16 && t.count == 1 && t.bytes == 16, "first sight: a new, invalid copy");
	const unsigned char one[4] = { 1, 2, 3, 4 };
	bool differs = false;
	Check(UploadShadowUpdate(a, 4, one, 4, &differs) && differs, "first store differs from the zeroed copy");
	a->valid = true;
	differs = false;
	Check(UploadShadowUpdate(a, 4, one, 4, &differs) && !differs, "same bytes: identical");
	const unsigned char two[4] = { 1, 2, 3, 5 };
	Check(UploadShadowUpdate(a, 4, two, 4, &differs) && differs && a->bytes[7] == 5, "changed bytes differ and are stored");
	differs = false;
	Check(!UploadShadowUpdate(a, 14, one, 4, &differs) && !differs && a->bytes[14] == 0, "a range past the copy is refused, untouched");
	Check(!UploadShadowUpdate(a, 20, one, 0, &differs), "an offset past the copy is refused");
	Check(UploadShadowUpdate(a, 12, one, 4, &differs), "a range ending at the copy's end is accepted");

	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == a && a->valid, "same buffer, owner and size: the same valid copy");
	Check(t.Acquire(FakePtr(1), FakePtr(101), 16) == a && !a->valid, "owner change restarts the copy");
	a->valid = true;
	Check(t.Acquire(FakePtr(1), FakePtr(101), 32) == a && !a->valid && a->size == 32 && t.bytes == 32,
	      "size change restarts and resizes the copy");

	Check(t.Acquire(FakePtr(2), FakePtr(200), 32) != NULL && t.bytes == 64, "second buffer fills the byte cap");
	Check(t.Acquire(FakePtr(3), FakePtr(300), 1) == NULL && t.count == 2, "over the byte cap: refused");
	a->valid = true;
	Check(t.Acquire(FakePtr(1), FakePtr(101), 40) == NULL && a->valid && a->size == 32,
	      "a growth over the byte cap is refused, the copy kept");
	Check(t.Acquire(FakePtr(1), FakePtr(101), 8) == a && t.bytes == 40, "a shrink returns bytes to the cap");

	t.Configure(8, 1 << 20);
	int added = 0;
	for (int i = 0; i < 8; ++i)
		if (t.Acquire(FakePtr(10 + i), FakePtr(1000), 4))
			++added;
	Check(added == 6 && t.count == 6, "fill stops at 3/4 of the slots");
	for (int i = 0; i < 6; ++i)
	{
		UploadShadow* s = t.Acquire(FakePtr(10 + i), FakePtr(1000), 4);
		Check(s && s->buffer == FakePtr(10 + i), "every added buffer is still found");
	}
	t.Clear();
	Check(t.slots == NULL && t.count == 0 && t.bytes == 0 && t.capacity == 8, "clear frees everything, keeps the configuration");
	Check(t.Acquire(FakePtr(10), FakePtr(1000), 4) != NULL && !t.Acquire(FakePtr(10), FakePtr(1000), 4)->valid,
	      "after a clear every buffer is first sight again");
	Check(t.Acquire(NULL, FakePtr(1000), 4) == NULL, "a NULL buffer is refused");
	t.Clear();

	// InvalidateCopies restarts every copy and keeps the plans; InvalidateAll
	// restarts both. Neither frees anything while the table is at most half full.
	t.Configure(8, 1024);
	UploadShadow* b = t.Acquire(FakePtr(1), FakePtr(100), 16);
	UploadShadow* c = t.Acquire(FakePtr(2), FakePtr(200), 16);
	b->valid = c->valid = true;
	b->planValid = c->planValid = true;
	t.InvalidateCopies();
	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == b && !b->valid && b->planValid,
	      "a copies-only invalidation restarts the copy and keeps the plan");
	b->valid = true;
	b->planRefused = true;
	b->planValid = false;
	t.InvalidateCopies();
	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == b && b->planRefused, "a copies-only invalidation keeps a refusal");
	b->planRefused = false;
	b->planValid = true;
	b->valid = c->valid = true;
	c->planValid = true;
	t.InvalidateAll();
	Check(t.count == 2 && t.bytes == 32, "invalidate frees nothing");
	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == b && !b->valid && !b->planValid, "invalidate restarts a copy and its plan");
	b->valid = true;
	b->planValid = true;
	Check(t.Acquire(FakePtr(1), FakePtr(100), 16) == b && b->valid && b->planValid, "a restarted copy stays until the next invalidate");
	Check(c->valid && t.Acquire(FakePtr(2), FakePtr(200), 16) == c && !c->valid && !c->planValid,
	      "every copy restarts, each at its own next acquire");
	c->planValid = true;
	Check(t.Acquire(FakePtr(2), FakePtr(201), 16) == c && !c->planValid, "an owner change drops the plan");

	// Plan room counts against the byte cap and survives a restart.
	Check(t.ReservePlan(b, 4) && b->planCap == 4 && t.bytes == 32 + 4 * sizeof(UploadPlanStep),
	      "plan room is counted in the byte cap");
	b->planCount = 3;
	b->planValid = true;
	Check(t.ReservePlan(b, 2) && b->planCap == 4 && b->planCount == 0 && !b->planValid &&
	      t.bytes == 32 + 4 * sizeof(UploadPlanStep), "a smaller plan reuses the room, emptied");
	Check(!t.ReservePlan(b, 1000) && b->planCap == 4 && b->planCount == 0 && !b->planValid,
	      "a plan over the byte cap is refused, the room kept");
	b->planRefused = true;
	t.InvalidateAll();
	t.Acquire(FakePtr(1), FakePtr(100), 16);
	Check(b->planCap == 4 && b->plan != NULL && !b->planRefused, "a restart keeps the plan room and drops a refusal");
	b->valid = true;
	b->planRefused = true;
	Check(t.ReservePlan(b, 1) && !b->valid && !b->planRefused, "a new plan restarts the copy and clears a refusal");
	t.Clear();
	Check(t.bytes == 0 && t.slots == NULL, "clear frees the plans too");

	// Past half full, an invalidation gives every slot back and keeps the array.
	t.Configure(8, 1024);
	for (int i = 0; i < 4; ++i)
		t.Acquire(FakePtr(20 + i), FakePtr(2000), 8);
	UploadShadow* keptArray = t.slots;
	t.InvalidateCopies();
	Check(t.count == 4 && t.bytes == 32, "half full: nothing reclaimed");
	t.Acquire(FakePtr(24), FakePtr(2000), 8);
	Check(t.count == 5, "one more than half");
	UploadShadow* e = t.Acquire(FakePtr(20), FakePtr(2000), 8);
	t.ReservePlan(e, 2);
	t.InvalidateCopies();
	Check(t.count == 0 && t.bytes == 0 && t.slots == keptArray, "crowded: every copy and plan freed, the slot array kept");
	UploadShadow* f = t.Acquire(FakePtr(20), FakePtr(2000), 8);
	Check(f && !f->valid && !f->planValid && f->planCap == 0 && t.count == 1, "after a reclaim every buffer is first sight");
	for (int i = 1; i < 5; ++i)
		t.Acquire(FakePtr(20 + i), FakePtr(2000), 8);
	t.InvalidateAll();
	Check(t.count == 0 && t.bytes == 0, "InvalidateAll reclaims too");
	t.Clear();
}

static bool BuildTestPlan(UploadShadowTable* t, UploadShadow* s, const FakeDef* defs, int n,
                          const size_t* dst, const size_t* size, const void* map, long mapGen)
{
	if (!t->ReservePlan(s, n))
		return false;
	for (int i = 0; i < n; ++i)
		if (!UploadPlanAdd(s, dst[i], size[i], &defs[i], defs[i].type, defs[i].type == 1))
			return false;
	UploadPlanCommit(s, map, FakePtr(500), FakePtr(501), mapGen);
	return true;
}

static void UploadPlanTests()
{
	Check(UPLOAD_DEF_TYPE == offsetof(FakeDef, type) && UPLOAD_DEF_PHYS_INDEX == offsetof(FakeDef, physicalIndex),
	      "definition offsets match the stand-in");

	UploadShadowTable t;
	memset(&t, 0, sizeof(t));
	t.Configure(8, 4096);
	UploadShadow* s = t.Acquire(FakePtr(1), FakePtr(100), 32);

	// Type 1 reads the float array, anything else the int array.
	float floats[16];
	int ints[8];
	for (int i = 0; i < 16; ++i)
		floats[i] = (float)i;
	for (int i = 0; i < 8; ++i)
		ints[i] = 100 + i;
	FakeDef defs[3] = { { 1, 4 }, { 2, 2 }, { 1, 0 } };
	const size_t dst[3] = { 0, 16, 24 };
	const size_t size[3] = { 16, 8, 8 };
	const void* map = FakePtr(300);
	Check(!UploadPlanMatches(s, map, FakePtr(500), FakePtr(501), 7), "a new copy has no plan");
	Check(BuildTestPlan(&t, s, defs, 3, dst, size, map, 7), "plan built");
	Check(UploadPlanMatches(s, map, FakePtr(500), FakePtr(501), 7), "the plan matches its key");
	Check(!UploadPlanMatches(s, FakePtr(301), FakePtr(500), FakePtr(501), 7), "another map: no match");
	Check(!UploadPlanMatches(s, map, FakePtr(502), FakePtr(501), 7), "other variable records: no match");
	Check(!UploadPlanMatches(s, map, FakePtr(500), FakePtr(503), 7), "a changed record count: no match");
	Check(!UploadPlanMatches(s, map, FakePtr(500), FakePtr(501), 8), "a map deleted since: no match");

	const char* f = (const char*)floats;
	const char* in = (const char*)ints;
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS, "first sight differs, whatever the bytes");
	float expectF[4] = { 4, 5, 6, 7 };
	int expectI[2] = { 102, 103 };
	float expectF2[2] = { 0, 1 };
	Check(memcmp(s->bytes, expectF, 16) == 0 && memcmp(s->bytes + 16, expectI, 8) == 0 &&
	      memcmp(s->bytes + 24, expectF2, 8) == 0, "the copy holds each step's source bytes");
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS && !s->valid, "an invalid copy never compares same");
	s->valid = true;
	Check(UploadPlanCompare(s, f, in) == UC_SAME, "valid copy, same bytes: same");
	ints[3] = 7;
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS && memcmp(s->bytes + 20, &ints[3], 4) == 0 && s->valid,
	      "a changed int differs and is stored; valid is left to the caller");
	Check(UploadPlanCompare(s, f, in) == UC_SAME, "stored bytes compare same next time");
	float moved[16];
	memcpy(moved, floats, sizeof(floats));
	Check(UploadPlanCompare(s, (const char*)moved, in) == UC_SAME, "a reallocated array with the same values: same");
	defs[0].physicalIndex = 8;
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS && s->bytes[0] == ((const unsigned char*)&floats[8])[0],
	      "the current physical index is read on every compare");
	defs[1].type = 1;
	Check(UploadPlanCompare(s, f, in) == UC_STALE && !s->planValid && !s->valid, "a changed type: plan and copy stale");

	// A rebuilt plan never trusts the old copy: a wider plan covers bytes it never took.
	t.ReservePlan(s, 1);
	UploadPlanAdd(s, 0, 4, &defs[2], 1, true);
	UploadPlanCommit(s, map, FakePtr(500), FakePtr(501), 9);
	UploadPlanCompare(s, f, in);
	s->valid = true;
	Check(UploadPlanCompare(s, f, in) == UC_SAME, "narrow plan: same");
	memset(s->bytes + 4, 0, 4);   // bytes the narrow plan never took; the new step's source (floats[0]) is 0 too
	Check(t.ReservePlan(s, 2) && UploadPlanAdd(s, 0, 4, &defs[2], 1, true) && UploadPlanAdd(s, 4, 4, &defs[2], 1, true),
	      "wider plan built");
	UploadPlanCommit(s, map, FakePtr(500), FakePtr(501), 9);
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS, "the first compare after a rebuild differs");

	// A refused key is latched until the key changes.
	UploadPlanRefuse(s, map, FakePtr(500), FakePtr(501), 10);
	Check(!s->planValid && UploadPlanRefusedFor(s, map, FakePtr(500), FakePtr(501), 10) &&
	      !UploadPlanMatches(s, map, FakePtr(500), FakePtr(501), 10), "refused: latched, no plan");
	Check(!UploadPlanRefusedFor(s, map, FakePtr(500), FakePtr(501), 11) &&
	      !UploadPlanRefusedFor(s, FakePtr(301), FakePtr(500), FakePtr(501), 10) &&
	      !UploadPlanRefusedFor(s, map, FakePtr(500), FakePtr(502), 10), "another key is not refused");
	UploadPlanCommit(s, map, FakePtr(500), FakePtr(501), 12);
	Check(!s->planRefused, "a committed plan clears the refusal");

	// Destination checks at build time.
	Check(t.ReservePlan(s, 2) && s->planCap == 3, "room for two keeps the room for three");
	Check(UploadPlanAdd(s, 24, 8, &defs[0], 1, true), "a step ending at the copy's end fits");
	Check(!UploadPlanAdd(s, 28, 8, &defs[0], 1, true), "a step past the copy's end is refused");
	Check(!UploadPlanAdd(s, 40, 0, &defs[0], 1, true), "a step starting past the copy is refused");
	Check(UploadPlanAdd(s, 0, 4, &defs[0], 1, true) && UploadPlanAdd(s, 4, 4, &defs[0], 1, true) &&
	      !UploadPlanAdd(s, 8, 4, &defs[0], 1, true), "no step beyond the plan's room");
	Check(!s->planValid, "an uncommitted plan never matches");

	// An empty plan compares same once the copy is valid.
	Check(t.ReservePlan(s, 0), "room for none");
	UploadPlanCommit(s, map, FakePtr(500), FakePtr(500), 7);
	s->valid = false;
	Check(UploadPlanCompare(s, f, in) == UC_DIFFERS, "empty plan, invalid copy: differs");
	s->valid = true;
	Check(UploadPlanCompare(s, f, in) == UC_SAME, "empty plan, valid copy: same");
	t.Clear();
}

static void GpuParamCacheTests()
{
	FakeNode probe;
	Check(sizeof(std::string) == 40 && sizeof(MsvcString) == 32, "std::string is 40 bytes, MsvcString its first 32");
	Check((size_t)((char*)&probe.def - (char*)&probe.key) == GPC_KEY_TO_DEF, "definition 0x28 bytes after the key");
	{
		std::string shortStr("worldViewProj");
		std::string longStr("lightSpecularColourPowerScaled");
		const MsvcString* s = (const MsvcString*)&shortStr;
		const MsvcString* l = (const MsvcString*)&longStr;
		Check(s->size == shortStr.size() && memcmp(s->Data(), "worldViewProj", 13) == 0, "MsvcString reads an in-place string");
		Check(l->size == longStr.size() && l->Data() == longStr.c_str(), "MsvcString reads a heap string");
	}

	// Name objects at fixed addresses with the D3D11 description's 0x38 stride,
	// and fixed map addresses (the table never dereferences a map), so the
	// hash layout and every count below are the same on every run.
	static GpuParamCache cache;
	const int N = 5000, MAPS = 5;
	const size_t NAME_STRIDE = 0x38;
	char* nameBase = (char*)VirtualAlloc((void*)0x0000000400000000ull, N * NAME_STRIDE,
	                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	Check(nameBase == (char*)0x0000000400000000ull, "name block at its fixed address");
	if (!nameBase)
		return;
	const void* maps[MAPS];
	for (int m = 0; m < MAPS; ++m)
		maps[m] = (const void*)(0x0000000500000000ull + m * 0x48);
	std::vector<FakeNode> nodes(N);
	std::vector<std::string> longText(N);   // heap text of the long names
	std::vector<MsvcString*> names(N);
	for (int i = 0; i < N; ++i)
	{
		std::string text = ConstName(i);
		nodes[i].key = text;
		nodes[i].def.type = i;
		MsvcString* s = (MsvcString*)(nameBase + i * NAME_STRIDE);
		s->size = text.size();
		if (text.size() < 16)
		{
			memcpy(s->bx.buf, text.c_str(), text.size() + 1);
			s->res = 15;
		}
		else
		{
			longText[i] = text;
			s->bx.ptr = longText[i].c_str();
			s->res = text.size();
		}
		names[i] = s;
	}

	// More keys than slots: whatever is found must be the right definition.
	for (int i = 0; i < N; ++i)
		cache.Store(maps[i % MAPS], names[i], &nodes[i].def);
	int hits = 0, misses = 0, wrong = 0;
	for (int i = 0; i < N; ++i)
	{
		const void* d = cache.Find(maps[i % MAPS], names[i]);
		if (!d) ++misses;
		else if (d == &nodes[i].def) ++hits;
		else ++wrong;
	}
	printf("gpu param cache: %d keys, %d hits, %d misses\n", N, hits, misses);
	Check(wrong == 0, "5000 keys: no lookup returns another key's definition");
	Check(hits == HITS_5000 && misses == N - HITS_5000, "5000 keys: the fixed layout keeps the same keys every run");
	Check(cache.Find(maps[1], names[0]) == NULL, "same name under another map misses");

	// Half full, nearly every key is kept.
	cache.Clear();
	Check(cache.Find(maps[0], names[0]) == NULL, "clear empties the table");
	const int M = 2000;
	for (int i = 0; i < M; ++i)
		cache.Store(maps[i % 2], names[i], &nodes[i].def);
	hits = 0;
	for (int i = 0; i < M; ++i)
		if (cache.Find(maps[i % 2], names[i]) == &nodes[i].def)
			++hits;
	Check(hits == HITS_2000, "2000 keys: the fixed layout keeps the same keys every run");

	// Validation: the requested name's text, and the node's own key.
	longText[0][5] = 'X';                   // heap text changed in place
	Check(cache.Find(maps[0], names[0]) == NULL, "changed long name misses");
	longText[0][5] = ConstName(0)[5];
	Check(cache.Find(maps[0], names[0]) == &nodes[0].def, "restored long name hits");
	names[1]->bx.buf[1] = '9';              // in-place text changed
	Check(cache.Find(maps[1], names[1]) == NULL, "changed short name misses");
	names[1]->bx.buf[1] = '1';
	names[4]->bx.buf[2] = 'x';              // "c4" -> "c4x", same object
	names[4]->bx.buf[3] = 0;
	names[4]->size = 3;
	Check(cache.Find(maps[0], names[4]) == NULL, "longer name misses");
	names[4]->bx.buf[2] = 0;
	names[4]->size = 2;
	Check(cache.Find(maps[0], names[4]) == &nodes[4].def, "restored short name hits");
	nodes[2].key[1] = 'Z';                  // the node's key no longer matches
	Check(cache.Find(maps[0], names[2]) == NULL, "changed node key misses");
	nodes[2].key = ConstName(2);
	Check(cache.Find(maps[0], names[2]) == &nodes[2].def, "restored node key hits");
	cache.Store(maps[1], names[1], &nodes[1].def);
	Check(cache.Find(maps[1], names[1]) == &nodes[1].def, "rewritten entry hits");

	// Invalidation by map drops exactly that map's entries.
	cache.Store(maps[0], names[0], &nodes[0].def);   // a rewrite, not a second entry
	int kept[2] = { 0, 0 };
	for (int i = 0; i < M; ++i)
		if (cache.Find(maps[i % 2], names[i]) == &nodes[i].def)
			++kept[i % 2];
	Check(kept[0] + kept[1] == HITS_2000, "validation left the table as it was");
	Check(cache.DropMap(maps[0]) == kept[0], "drop removes every entry of the map, once");
	int left0 = 0, left1 = 0;
	for (int i = 0; i < M; ++i)
	{
		const void* d = cache.Find(maps[i % 2], names[i]);
		if (d && i % 2 == 0) ++left0;
		if (d == &nodes[i].def && i % 2 == 1) ++left1;
	}
	Check(left0 == 0 && left1 == kept[1], "dropped map misses, the other map still hits");
	Check(cache.DropMap(maps[0]) == 0, "second drop finds nothing");
	VirtualFree(nameBase, 0, MEM_RELEASE);
}

static void IniTemplateTests()
{
	std::ifstream f("KEO.ini", std::ios::binary);
	Check(f.is_open(), "KEO.ini found at repo root (build_tests.bat's cwd)");
	if (!f.is_open())
		return;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	// Every line is inert: not a live key=value line SplitIniLine would act on.
	{
		bool allInert = true;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t nl = text.find('\n', pos);
			std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos = nl == std::string::npos ? text.size() : nl + 1;
			std::string key, val;
			if (SplitIniLine(line, &key, &val))
				allInert = false;
		}
		Check(allInert, "the template has no live key=value line (every line is a comment, a section header, or blank)");
	}

	// No documented "# key=value" line names the same key twice: two commented
	// defaults for one key is a documentation bug in its own right, and it
	// would also mean the file has a live duplicate the moment both lines are
	// ever uncommented at once.
	{
		std::vector<IniDupSeen> seen;
		int line = 0;
		size_t pos = 0;
		while (pos < text.size())
		{
			++line;
			size_t nl = text.find('\n', pos);
			std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos = nl == std::string::npos ? text.size() : nl + 1;
			std::string trimmed = IniTrim(raw);
			if (trimmed.empty() || trimmed[0] != '#')
				continue;
			std::string key, val;
			if (!SplitIniLine(IniTrim(trimmed.substr(1)), &key, &val))
				continue;
			IniNoteAppliedKey(seen, key, line);
		}
		bool anyDup = false;
		for (size_t i = 0; i < seen.size(); ++i)
			if (seen[i].firstLine != seen[i].lastLine)
				anyDup = true;
		Check(!anyDup, "the template documents no key twice");
	}

	// Loaded through the same parse path LoadConfig uses (SplitIniLine then
	// ParseRenderKey per line): yields exactly the compiled render defaults.
	{
		RenderConfig parsed = ParseRenderText(text, RenderConfigDefaults());
		Check(AllKeysEqual(parsed, RenderConfigDefaults()), "the template parses back to the compiled render defaults");
	}

	// Every "# key=value" line's documented default matches the compiled
	// one, and every render key has such a line. Starts from garbage so a
	// missing or stale documented default shows up as a mismatch, not a
	// coincidental pass.
	{
		RenderConfig fromComments;
		memset(&fromComments, 0xAA, sizeof(fromComments));
		fromComments.particleLoopingNames[0] = 0;
		int found = 0;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t nl = text.find('\n', pos);
			std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos = nl == std::string::npos ? text.size() : nl + 1;
			std::string trimmed = IniTrim(line);
			if (trimmed.empty() || trimmed[0] != '#')
				continue;
			std::string key, val;
			if (!SplitIniLine(IniTrim(trimmed.substr(1)), &key, &val))
				continue;
			if (ParseRenderKey(&fromComments, key, val) == RP_OK)
				++found;
		}
		int total = 0;
		for (int i = 0; g_renderKeys[i].name; ++i)
			++total;
		Check(found == total, "every render key in the table has a documented '# key=value' default line");
		Check(AllKeysEqual(fromComments, RenderConfigDefaults()), "every documented default matches the compiled default");
	}

	// A settings-panel save with one changed key keeps every template line
	// (comments included) and appends only that key.
	{
		RenderConfig defaults = RenderConfigDefaults();
		RenderConfig cfg = defaults;
		cfg.particleStepCap = !defaults.particleStepCap;
		std::string expected = text + "particleStepCap=" + (cfg.particleStepCap ? "true" : "false") + FirstEol(text);
		std::string out = RewriteRenderIni(text, cfg, defaults);
		CheckText(out, expected, "rewrite over the template: every template line kept, one changed key appended");
	}

	// The Options tab's worker count lands above [Render], outside every section.
	{
		std::vector<IniEntry> e(1, Entry("navmeshWorkerCount", "4", INI_INT, true));
		std::string out = RewriteIniKeys(text, e, NULL);
		size_t key = out.find("\nnavmeshWorkerCount=4");
		size_t firstSection = out.find("\n[");
		Check(key != std::string::npos && firstSection != std::string::npos && key < firstSection,
		      "worker count saved over the template goes before the first section");
		Check(out.size() == text.size() + strlen("navmeshWorkerCount=4") + FirstEol(text).size(),
		      "worker count saved over the template adds one line only");
	}

	// The DEV-only diagnostics are off by default in every build.
	Check(!RenderConfigDefaults().shadowReachDiag && !RenderConfigDefaults().gpuParamLookupDiag &&
	      !RenderConfigDefaults().oldAnimDiag && !RenderConfigDefaults().gpuUploadDiag, "diagnostics default off");
	Check(RenderConfigDefaults().oldAnimSkip && RenderConfigDefaults().shadowReachCull, "oldAnimSkip and shadowReachCull default on");
	Check(RenderConfigDefaults().gpuUploadSkip, "gpuUploadSkip defaults on");
}

// One frame of n calls, each costing costMs; ran[i] gets 1 for an admitted call.
static int RunFoliageFrame(FoliageBudget* b, int n, double costMs, bool active, double budgetMs, int* ran)
{
	int admitted = 0;
	for (int i = 0; i < n; ++i)
	{
		ran[i] = FoliageBudgetAdmit(b, active, budgetMs) ? 1 : 0;
		if (ran[i])
		{
			FoliageBudgetSpend(b, costMs);
			++admitted;
		}
	}
	FoliageBudgetEndFrame(b);
	return admitted;
}

static void FoliageBudgetTests()
{
	FoliageBudget b;
	int ran[16];

	FoliageBudgetReset(&b);
	Check(RunFoliageFrame(&b, 7, 5.0, false, 4.0, ran) == 7 && b.start == 0, "unbudgeted frame runs every call");
	Check(b.frameMaxMs == 35.0, "frame total recorded");

	// 1 ms calls, 2.5 ms budget: call 0 plus two more a frame, every call
	// within ceil(6 / 2) + 1 frames of its last run, and never a frame with none.
	FoliageBudgetReset(&b);
	int lastRun[7];
	for (int i = 0; i < 7; ++i)
		lastRun[i] = -1;
	bool fair = true, progress = true;
	for (int f = 0; f < 40; ++f)
	{
		if (RunFoliageFrame(&b, 7, 1.0, true, 2.5, ran) < 1 || !ran[0])
			progress = false;
		for (int i = 0; i < 7; ++i)
		{
			if (ran[i])
				lastRun[i] = f;
			else if (f - lastRun[i] > 4)
				fair = false;
		}
	}
	Check(progress && fair, "budgeted rotation serves every call within ceil(N/B)+1 frames");

	// Calls over the budget still run: call 0 and the rotating start each frame.
	FoliageBudgetReset(&b);
	bool oneEach = true;
	for (int f = 0; f < 10; ++f)
	{
		RunFoliageFrame(&b, 5, 30.0, true, 4.0, ran);
		for (int i = 0; i < 5; ++i)
			if (ran[i] != (i == 0 || i == f % 5 ? 1 : 0))
				oneEach = false;
	}
	Check(oneEach, "slow calls: call 0 plus the rotating start, one step a frame");

	// The list shrinks below start: that frame still runs call 0, the next starts at 0.
	FoliageBudgetReset(&b);
	b.start = 5;
	Check(RunFoliageFrame(&b, 3, 1.0, true, 4.0, ran) == 1 && ran[0] && b.start == 0, "shrunk list: call 0 runs, start resets");
	Check(RunFoliageFrame(&b, 3, 1.0, true, 4.0, ran) == 3, "then runs from the top");

	// Leaving the budget (speed back down, key off) runs everything and resets start.
	FoliageBudgetReset(&b);
	RunFoliageFrame(&b, 7, 3.0, true, 4.0, ran);
	Check(b.start == 2, "budget stops after the call that crossed it");
	Check(RunFoliageFrame(&b, 7, 3.0, false, 4.0, ran) == 7 && b.start == 0, "unbudgeted frame resets start");
}

static PtrSet<64> g_testSet;   // static storage: starts zeroed, i.e. empty

static void PtrSetTests()
{
	Check(g_testSet.count == 0 && !g_testSet.Contains((void*)0x1000), "a static set starts empty");

	// Random adds and removes against a plain presence array; 16-byte
	// aligned addresses from a small range, so chains collide and wrap.
	bool ref[256];
	memset(ref, 0, sizeof(ref));
	int refCount = 0;
	unsigned rng = 12345;
	bool agree = true;
	for (int step = 0; step < 20000 && agree; ++step)
	{
		rng = rng * 1103515245u + 12345u;
		int k = (int)((rng >> 16) % 256);
		const void* p = (const void*)(size_t)(0x10000 + k * 16);
		if ((rng >> 8) & 1)
		{
			bool added = g_testSet.Add(p);
			if (!ref[k] && added) { ref[k] = true; ++refCount; }
			if (!ref[k] && !added && refCount < PtrSet<64>::MAX_FILL) agree = false;
		}
		else
		{
			g_testSet.Remove(p);
			if (ref[k]) { ref[k] = false; --refCount; }
		}
		if (g_testSet.count != refCount)
			agree = false;
		for (int j = 0; j < 256 && agree; ++j)
		{
			if (g_testSet.Contains((const void*)(size_t)(0x10000 + j * 16)) != ref[j])
				agree = false;
		}
	}
	Check(agree, "set agrees with the reference through 20000 adds and removes");

	g_testSet.Clear();
	int added = 0;
	for (int k = 0; k < 100; ++k)
		added += g_testSet.Add((const void*)(size_t)(0x20000 + k * 16)) ? 1 : 0;
	Check(added == PtrSet<64>::MAX_FILL && g_testSet.count == PtrSet<64>::MAX_FILL, "fill stops at three quarters");
	Check(g_testSet.Add((const void*)(size_t)0x20000), "a member is still reported added when full");
	g_testSet.Remove((const void*)(size_t)0x20000);
	Check(g_testSet.Add((const void*)(size_t)(0x20000 + 99 * 16)), "a removal makes room");
	Check(!g_testSet.Contains(NULL), "NULL is never a member");
}

static const SettingsRow* FindRow(const std::vector<SettingsRow>& rows, const char* labelStart)
{
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].label.compare(0, strlen(labelStart), labelStart) == 0)
			return &rows[i];
	}
	return NULL;
}

static size_t SettingRows(const std::vector<SettingsRow>& rows)
{
	size_t n = 0;
	for (size_t i = 0; i < rows.size(); ++i)
		n += rows[i].kind == SR_HEADER ? 0 : 1;
	return n;
}

static void BenchRowsTests(SettingsStaging st, size_t baseRows, size_t prodRows)
{
	BenchSlot slots[BENCH_SLOT_COUNT];
	memset(slots, 0, sizeof(slots));
	slots[1].recorded = true;
	slots[1].speed = 20;
	slots[1].hour = 13.4f;
	StageBenchSpeeds(&st, slots);
	Check(st.benchSpeed[0] == 1 && st.benchSpeed[1] == 20, "staged speed: 1 without a valid speed, else the slot's");

	SettingsBench bench;
	Check(bench.sweepLeg == 0 && bench.sweepLegs == 0 && bench.activeSlot == -1 && bench.runSec == 0.0 && !bench.slots,
	      "SettingsBench defaults");
	bench.available = true;
	bench.slots = slots;
	bench.activeSlot = 2;
	std::vector<SettingsRow> rows;
	BuildSettingsRows(&st, false, &bench, &rows);
	Check(rows.size() == prodRows && FindRow(rows, "Benchmark") == NULL, "PROD: no Benchmark section");
	rows.clear();
	BuildSettingsRows(&st, true, &bench, &rows);
	Check(rows.size() == baseRows + 1 + 4 * BENCH_SLOT_COUNT + 1, "Benchmark: a header, four rows per slot, the sweep");
	const SettingsRow* h = baseRows < rows.size() ? &rows[baseRows] : NULL;
	Check(h && h->kind == SR_HEADER && h->label == "Benchmark", "the section follows the existing rows");
	for (int i = 0; i < BENCH_SLOT_COUNT && baseRows + 4 + 4 * i < rows.size(); ++i)
	{
		const SettingsRow* r = &rows[baseRows + 1 + 4 * i];
		Check(r[0].kind == SR_TEXT && r[1].kind == SR_DROPBOX && r[2].kind == SR_BUTTON && r[3].kind == SR_BUTTON,
		      "slot rows: text, speed, Record, Run");
		Check(r[1].intPtr == &st.benchSpeed[i] && r[1].choices.size() == 2 && r[1].choices[0].second == 1
		      && r[1].choices[1].first == "20x" && r[1].choices[1].second == 20, "speed drop box bound to staging, 1x/20x");
		Check(r[2].buttonId == BENCH_BUTTON_RECORD + i && r[2].caption == "Record here", "Record id and caption");
		Check(r[3].buttonId == BENCH_BUTTON_RUN + i && r[3].caption == (i == 2 ? "Stop" : "Run"),
		      "Run id; Stop while that slot runs");
	}
	Check(FindRow(rows, "Swamp: not recorded") != NULL, "unrecorded slot text");
	Check(BENCH_SLOT_COUNT == 5 && FindRow(rows, "Sand: not recorded") != NULL, "the Sand slot's rows");
	const SettingsRow* sandRun = FindRow(rows, "Sand benchmark");
	Check(sandRun && sandRun->buttonId == BENCH_BUTTON_RUN + 4, "Sand Run id");
	const SettingsRow& sweep = rows.back();
	Check(sweep.kind == SR_BUTTON && sweep.buttonId == BENCH_BUTTON_SWEEP && sweep.caption == "Full sweep" &&
	      !sweep.tooltip.empty(), "the sweep button follows the slots");
	Check(BENCH_BUTTON_SWEEP >= BENCH_BUTTON_RUN + 100 && BENCH_BUTTON_SWEEP % 100 == 0,
	      "the sweep id is outside every slot's Record and Run ids");
	bench.sweepLeg = 3;
	bench.sweepLegs = 6;
	rows.clear();
	BuildSettingsRows(&st, true, &bench, &rows);
	Check(rows.back().caption == "Sweep 3/6 (stop)", "the sweep caption shows progress");
	Check(SweepCaption(0, 6) == "Full sweep" && SweepCaption(1, 6) == "Sweep 1/6 (stop)", "sweep caption");
	Check(rows.back().tooltip.find("minute") == std::string::npos &&
	      FindRow(rows, "Swamp benchmark")->tooltip.find("minute") == std::string::npos, "no durations when not given");
	bench.runSec = 574.0;
	bench.runSecCombined = 126.0;
	bench.sweepLegCount = 6;
	rows.clear();
	BuildSettingsRows(&st, true, &bench, &rows);
	Check(FindRow(rows, "Swamp benchmark")->tooltip.find("(about 10 minutes, 2 with bench.levers=combined)") != std::string::npos,
	      "Run tooltip: the run's duration, and combined's");
	Check(rows.back().tooltip.find("Its 6 legs take about 57 minutes, 13 with bench.levers=combined") != std::string::npos,
	      "sweep tooltip: the legs times the run");
	bench.runSec = 126.0;
	rows.clear();
	BuildSettingsRows(&st, true, &bench, &rows);
	Check(FindRow(rows, "Swamp benchmark")->tooltip.find("(about 2 minutes)") != std::string::npos,
	      "combined already on: one duration");
	Check(FindRow(rows, "City: speed 20x, hour 13.4") != NULL, "recorded slot text");
	bool unique = true;
	for (size_t i = 0; i < rows.size(); ++i)
		for (size_t j = i + 1; j < rows.size(); ++j)
			if (rows[i].label == rows[j].label)
				unique = false;
	Check(unique, "row labels are unique (the panel keys lines by label)");

	bench.available = false;
	bench.reason = "sky";
	rows.clear();
	BuildSettingsRows(&st, true, &bench, &rows);
	Check(rows.size() == baseRows + 2 && rows.back().kind == SR_TEXT
	      && rows.back().label == "Benchmark unavailable (sky)", "unavailable: one text line, no buttons");
}

static void SettingsRowsTests()
{
	// Every render key but the text list has a row label and a tooltip.
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (k.kind == RK_TEXT)
			Check(k.label == NULL, "text keys stay INI-only");
		else
			Check(k.label && k.tooltip && k.tooltip[0], "key has a label and a tooltip");
	}

	SettingsStaging st;
	memset(&st, 0, sizeof(st));
	int core = -1, workerKey = -1;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		StageModule(kConfigModules[m], &st.module[m]);
		if (strcmp(kConfigModules[m].name, "navmesh") == 0)
			core = m;
	}
	for (int i = 0; core >= 0 && kConfigModules[core].keys[i].name; ++i)
	{
		if (strcmp(kConfigModules[core].keys[i].name, "navmeshWorkerCount") == 0)
			workerKey = i;
	}
	Check(core >= 0 && workerKey >= 0, "the core module and its navmeshWorkerCount row");
	if (core < 0 || workerKey < 0)
		return;
	StagedRender(&st) = SampleRenderConfig();
	((navmesh::NavMeshConfig*)st.module[core].state)->cfg_navmeshWorkerCount = 0;
	std::vector<SettingsRow> dev, prod;
	BuildSettingsRows(&st, true, NULL, &dev);
	BuildSettingsRows(&st, false, NULL, &prod);
	size_t devOnlyRows = 0;
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		for (int i = 0; kConfigModules[m].keys[i].name; ++i)
		{
			const ConfigKey& k = kConfigModules[m].keys[i];
			bool widget = k.kind == CK_BOOL || k.kind == CK_FLOAT || k.kind == CK_DOUBLE
			           || (k.kind == CK_INT && (k.choices || k.lo <= k.hi)) || (k.kind == CK_CUSTOM && k.choices);
			if (k.devOnly && k.label && !k.retired && widget)
				++devOnlyRows;
		}
	}
	// A section with no PROD row has no PROD header either, so count rows only.
	Check(devOnlyRows >= 3 && SettingRows(dev) == SettingRows(prod) + devOnlyRows, "PROD hides the DEV-only rows");
	Check(FindRow(dev, "Shadow reach") != NULL && FindRow(prod, "Shadow reach") == NULL, "shadowReachDiag is DEV-only");
	Check(FindRow(prod, "Shader constant") == NULL, "gpuParamLookupDiag is DEV-only");
	Check(FindRow(dev, "Animation pass counter") != NULL && FindRow(prod, "Animation pass counter") == NULL,
	      "oldAnimDiag is DEV-only");
	Check(FindRow(dev, "Constant upload counter") != NULL && FindRow(prod, "Constant upload counter") == NULL,
	      "gpuUploadDiag is DEV-only");
	Check(FindRow(dev, "Skip idle character animation") != NULL && FindRow(prod, "Skip idle character animation") == NULL,
	      "oldAnimSkip is DEV-only");
	Check(FindRow(dev, "Skip unchanged shader constant") != NULL && FindRow(prod, "Skip unchanged shader constant") == NULL,
	      "gpuUploadSkip is DEV-only");
	Check(FindRow(dev, "Render stats") != NULL && FindRow(prod, "Render stats") == NULL, "renderDiag is DEV-only");

	const SettingsRow* levers = FindRow(dev, "Rendering optimizations");
	Check(!FindRow(prod, "Rendering optimizations"), "renderLevers: master control is DEV-page only");
	Check(levers && levers->label == "Rendering optimizations *" && levers->kind == SR_CHECKBOX
	      && levers->boolPtr == &StagedRender(&st).renderLevers, "renderLevers: a DEV checkbox bound to staging, marked restart");
	const SettingsRow* cap = FindRow(prod, "Cap particle updates");
	Check(cap && cap->label.find(" *") == std::string::npos && !cap->restart, "live keys are not marked restart");
	const SettingsRow* speed = FindRow(dev, "Particle step cap from");
	Check(speed && speed->kind == SR_SLIDER && speed->floatPtr == &StagedRender(&st).particleStepCapSpeed
	      && speed->lo == 1.5f && speed->hi == 20.0f && speed->stepExp == 1, "slider bound to staging with its drag grid");

	// The game's slider sits at lo + k / 2^stepExp. Each default, the slider's
	// ends and the clamp minimum's grid neighbour must be reachable by dragging.
	const RenderConfig& defaults = RenderConfigDefaults();
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (k.kind != RK_FLOAT)
			continue;
		float def = *(const float*)((const char*)&defaults + k.offset);
		float scale = (float)(1 << k.stepExp);
		double defSteps = ((double)def - k.sliderLo) * scale;
		double hiSteps = ((double)k.hi - k.sliderLo) * scale;
		Check(k.stepExp >= 0 && k.stepExp <= 8, "step exponent in range");
		Check(k.sliderLo >= k.lo && k.sliderLo <= def && def <= k.hi, "slider start within the clamp, at or below the default");
		Check(k.sliderLo - k.lo < 1.0f / scale, "slider starts on the first grid step at or above the clamp minimum");
		Check(defSteps == floor(defSteps), "default lies on the slider grid");
		Check(hiSteps == floor(hiSteps), "clamp maximum lies on the slider grid");
		Check((float)(defSteps / scale) + k.sliderLo == def, "the game's own position -> value math returns the default");
		if (defSteps != floor(defSteps) || hiSteps != floor(hiSteps))
			printf("  slider grid: %s\n", k.name);
	}
	Check(prod[0].kind == SR_HEADER && prod[0].label == "Zone loading", "the first player section comes first");

	const SettingsRow* wp = FindRow(prod, "Background navmesh threads");
	Check(wp != NULL, "the worker drop box is shown");
	if (!wp)
		return;
	const SettingsRow& w = *wp;
	Check(w.kind == SR_DROPBOX && w.intPtr == &((navmesh::NavMeshConfig*)st.module[core].state)->cfg_navmeshWorkerCount && w.label.find(" *") != std::string::npos && w.restart,
	      "worker drop box bound to staging, marked restart");
	Check(w.choices.size() == 7 && w.choices[0].first == "Auto" && w.choices[0].second == 0
	      && w.choices[6].first == "6" && w.choices[6].second == 6, "worker choices Auto, 1..capacity");

	// Diff: what a close applies and what it saves.
	{
		SettingsStaging saved = st, staged = st;
		RenderConfig live = StagedRender(&st);
		SettingsDiff d = DiffSettings(staged, live, saved);
		Check(d.applied == 0 && d.saved == 0, "no change, nothing to do");
		StagedRender(&staged).particleStepCap = !StagedRender(&staged).particleStepCap;
		StagedRender(&staged).particleOffscreenSeconds = 2.0f;
		d = DiffSettings(staged, live, saved);
		Check(d.applied == 2 && d.saved == 2, "two live keys: applied and saved");
		staged = st;
		StagedRender(&staged).renderLevers = !StagedRender(&staged).renderLevers;
		d = DiffSettings(staged, live, saved);
		Check(d.applied == 0 && d.saved == 1, "renderLevers: saved, never applied");
		staged = st;
		((navmesh::NavMeshConfig*)staged.module[core].state)->cfg_navmeshWorkerCount = 3;
		d = DiffSettings(staged, live, saved);
		Check(d.applied == 0 && d.saved == 1, "worker count: saved, never applied");
		staged = st;
		live.reflectionHalfRate = !live.reflectionHalfRate;
		d = DiffSettings(staged, live, saved);
		Check(d.applied == 1 && d.saved == 0, "a live value behind the INI is applied, not saved again");
	}

	{
		SettingsStaging saved = st, staged = st;
		((navmesh::NavMeshConfig*)staged.module[core].state)->cfg_navmeshWorkerCount = 4;
		std::vector<IniEntry> set, automatic;
		int n = ModuleStageEntries(kConfigModules[core], staged.module[core], saved.module[core], &set);
		Check(n == 1 && set.size() == 1, "worker entry: the one changed key");
		IniEntry e = set.empty() ? IniEntry() : set[0];
		Check(e.key == "navmeshWorkerCount" && e.value == "4" && e.kind == INI_INT && e.append, "worker entry appends a set count");
		((navmesh::NavMeshConfig*)saved.module[core].state)->cfg_navmeshWorkerCount = 4;
		((navmesh::NavMeshConfig*)staged.module[core].state)->cfg_navmeshWorkerCount = 0;
		n = ModuleStageEntries(kConfigModules[core], staged.module[core], saved.module[core], &automatic);
		Check(n == 1 && automatic.size() == 1 && !automatic[0].append, "automatic is not appended");
		CheckText(RewriteIniKeys("navmeshWorkerCount=2\n", automatic, NULL),
		          "navmeshWorkerCount=0\n", "worker line rewritten in place");
	}

	BenchRowsTests(st, dev.size(), prod.size());
}

int main()
{
	Check(AutoNavMeshWorkerCount(1, 6)  == 1, "1 cpu -> 1");
	Check(AutoNavMeshWorkerCount(4, 6)  == 2, "4 cpus -> 2");
	Check(AutoNavMeshWorkerCount(8, 6)  == 4, "8 cpus -> 4");
	Check(AutoNavMeshWorkerCount(16, 6) == 6, "16 cpus -> capacity 6");
	Check(AutoNavMeshWorkerCount(0, 6)  == 1, "0 cpus -> 1");

	{
		std::string path = GameRoot() + "\\RenderSystem_Direct3D11_x64.dll";
		HMODULE d3d = LoadLibraryExA(path.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
		Check(d3d != NULL, "load D3D11 render system");
		if (d3d)
		{
			// Slot RVA measured from the DLL's import table.
			void** slot = FindImportSlot(d3d, "OgreMain_x64.dll", GET_CONSTANT_DEFINITION);
			Check(slot != NULL, "getConstantDefinition import slot found");
			Check(slot && ((uintptr_t)slot - (uintptr_t)d3d) == 0x717F0, "slot RVA 0x717F0");
			Check(FindImportSlot(d3d, "ogremain_x64.DLL", GET_CONSTANT_DEFINITION) == slot,
			      "exporter name compared case-insensitively");
			Check(FindImportSlot(d3d, "OgreMain_x64.dll", "?noSuchSymbol@@YAXXZ") == NULL,
			      "missing symbol -> NULL");
			Check(FindImportSlot(d3d, "NoSuchModule.dll", GET_CONSTANT_DEFINITION) == NULL,
			      "missing exporter -> NULL");
			FreeLibrary(d3d);
		}
	}

	Check(FindImportSlot(NULL, "OgreMain_x64.dll", GET_CONSTANT_DEFINITION) == NULL,
	      "NULL importer -> NULL");
	{
		unsigned char garbage[64];
		memset(garbage, 0xCC, sizeof(garbage));
		Check(FindImportSlot((HMODULE)garbage, "OgreMain_x64.dll", GET_CONSTANT_DEFINITION) == NULL,
		      "non-PE buffer -> NULL");
	}
	{
		// A minimal image whose import directory RVA lies past SizeOfImage:
		// the bounds check must refuse it instead of reading past the buffer.
		unsigned char image[512];
		memset(image, 0, sizeof(image));
		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
		dos->e_magic = IMAGE_DOS_SIGNATURE;
		dos->e_lfanew = sizeof(IMAGE_DOS_HEADER);
		IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(image + dos->e_lfanew);
		nt->Signature = IMAGE_NT_SIGNATURE;
		nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
		nt->OptionalHeader.SizeOfImage = sizeof(image);
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = sizeof(image) + 0x1000;
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR);
		Check(FindImportSlot((HMODULE)image, "OgreMain_x64.dll", GET_CONSTANT_DEFINITION) == NULL,
		      "import directory RVA beyond SizeOfImage -> NULL");
	}

	{
		const char* list = "fire,smoke,torch,rain,weather";
		Check(NameMatchesList("fire1_small", list), "fire1_small is looping");
		Check(NameMatchesList("fire2_torch", list), "fire2_torch is looping");
		Check(NameMatchesList("smoke3_small", list), "smoke3_small is looping");
		Check(NameMatchesList("Kenshi_Heavy_Rain", list), "match is case-insensitive");
		Check(!NameMatchesList("Dustkick01", list), "Dustkick01 is not looping");
		Check(!NameMatchesList("5461-new_furniture_otto.mod", list), "furniture stringID does not match fire");
		Check(!NameMatchesList("", list), "empty name matches nothing");
		Check(!NameMatchesList("fire1_small", ""), "empty list matches nothing");
		Check(!NameMatchesList("fire1_small", " , ,"), "blank tokens match nothing");
		Check(NameMatchesList("sparks1", "  dust , SPARK "), "tokens are trimmed");
		Check(!NameMatchesList("fir", "fire"), "token longer than the name");
		Check(NameMatchesList("5461-new_furniture_otto.mod", "5461-new_furniture_otto.mod"), "a stringID can be listed");
	}

	{
		std::string root = GameRoot();
		HMODULE pu = LoadLibraryExA((root + "\\Plugin_ParticleUniverse_x64.dll").c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
		HMODULE ogre = LoadLibraryExA((root + "\\OgreMain_x64.dll").c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
		Check(pu != NULL && ogre != NULL, "load ParticleUniverse and OgreMain");
		if (pu && ogre)
		{
			Check(VerifyParticleLayout(pu, ogre), "particle layout verifies against the shipped DLLs");
			Check(!VerifyParticleLayout(ogre, ogre), "particle layout refuses the wrong module");
			Check(!VerifyParticleLayout(pu, NULL), "particle layout refuses a missing OgreMain");
		}
		if (pu) FreeLibrary(pu);
		if (ogre) FreeLibrary(ogre);
	}
	{
		std::string shortStr("fire1_small");
		std::string longStr("Desert-dust-swirls EFFECT");
		char out[16];
		ReadStdString(&shortStr, out, sizeof(out));
		Check(strcmp(out, "fire1_small") == 0, "std::string read, in-place buffer");
		ReadStdString(&longStr, out, sizeof(out));
		Check(strcmp(out, "Desert-dust-swi") == 0, "std::string read, heap buffer, truncated");
		unsigned char junk[40];
		memset(junk, 0xFF, sizeof(junk));
		ReadStdString(junk, out, sizeof(out));
		Check(out[0] == 0, "garbage is not a string");
	}

	{
		const float MIN_AGE = 10.0f;
		const unsigned INF_BITS = 0x7F800000u, NAN_BITS = 0x7FC00000u;
		float inf, nan;
		memcpy(&inf, &INF_BITS, sizeof(inf));
		memcpy(&nan, &NAN_BITS, sizeof(nan));
		Check(OffscreenTimeoutAction(true, 0, false, 12.0f, MIN_AGE, false) == OFFSCREEN_SET, "settled looping effect gets the timeout");
		Check(OffscreenTimeoutAction(true, 0, true, 12.0f, MIN_AGE, false) == OFFSCREEN_NONE, "timeout already set is left");
		Check(OffscreenTimeoutAction(true, 0, false, 3.0f, MIN_AGE, false) == OFFSCREEN_NONE, "young effect is left alone");
		Check(OffscreenTimeoutAction(true, 0, true, 3.0f, MIN_AGE, false) == OFFSCREEN_CLEAR, "young effect carrying a timeout is cleared");
		Check(OffscreenTimeoutAction(true, 0, true, 40.0f, MIN_AGE, true) == OFFSCREEN_CLEAR, "restarted effect carrying a timeout is cleared");
		Check(OffscreenTimeoutAction(true, 0, false, 40.0f, MIN_AGE, true) == OFFSCREEN_NONE, "restarted effect waits a walk");
		Check(OffscreenTimeoutAction(false, 0, false, 40.0f, MIN_AGE, false) == OFFSCREEN_NONE, "non-looping effect is left alone");
		Check(OffscreenTimeoutAction(false, 0, true, 3.0f, MIN_AGE, false) == OFFSCREEN_NONE, "non-looping script timeout is left");
		Check(OffscreenTimeoutAction(true, 1, true, 40.0f, MIN_AGE, false) == OFFSCREEN_CLEAR, "stopping effect is cleared");
		Check(OffscreenTimeoutAction(false, 1, true, 1.0f, MIN_AGE, false) == OFFSCREEN_CLEAR, "stopping effect is cleared at any age");
		Check(OffscreenTimeoutAction(true, 1, false, 40.0f, MIN_AGE, false) == OFFSCREEN_NONE, "stopping effect without timeout is left");
		Check(OffscreenTimeoutAction(true, 2, false, 40.0f, MIN_AGE, false) == OFFSCREEN_NONE, "dead effect is left");
		Check(OffscreenTimeoutAction(true, 0, false, -1.0f, MIN_AGE, false) == OFFSCREEN_NONE, "negative age is implausible");
		Check(OffscreenTimeoutAction(true, 0, false, nan, MIN_AGE, false) == OFFSCREEN_NONE, "NaN age is implausible");
		Check(OffscreenTimeoutAction(true, 0, false, inf, MIN_AGE, false) == OFFSCREEN_NONE, "infinite age is implausible");
		Check(OffscreenTimeoutAction(true, 0, false, 0.0f, 0.0f, false) == OFFSCREEN_SET, "min age 0 sets at once");
		Check(OffscreenTimeoutAction(true, 0, false, 10.0f, MIN_AGE, false) == OFFSCREEN_SET, "age equal to min age sets");
		Check(OffscreenTimeoutAction(true, 0, true, 12.0f, MIN_AGE, false, true) == OFFSCREEN_SET, "stale timeout is set again");
		Check(OffscreenTimeoutAction(true, 0, true, 3.0f, MIN_AGE, false, true) == OFFSCREEN_CLEAR, "stale timeout on a young effect is cleared");
		Check(OffscreenTimeoutAction(true, 0, true, 40.0f, MIN_AGE, true, true) == OFFSCREEN_CLEAR, "stale timeout after a restart is cleared");
		Check(OffscreenTimeoutAction(true, 1, true, 40.0f, MIN_AGE, false, true) == OFFSCREEN_CLEAR, "stale timeout on a stopping effect is cleared");
	}

	ShadowReachTests();
	ShadowCullTests();
	RenderKeysTests();
	SettingsRowsTests();
	PtrSetTests();
	FoliageBudgetTests();
	IniTemplateTests();
	GpuParamCacheTests();
	IdStringTests();
	EmptyPassTests();
	CompositorLayoutTests();
	PassVisibilityTests();
	UploadShadowTests();
	UploadPlanTests();

	return CheckExit("render_units");
}
