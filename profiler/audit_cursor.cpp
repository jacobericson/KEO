// audit_cursor.cpp - Cursor ray and call-site timing.
// Main reads game memory; AI and physics probes write their own timing slots.
// No probe takes a lock, allocates, or logs.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {

// =========================================================================
// Call-site probes
// =========================================================================

// mainLoop metric each main-thread site feeds (-1: none).
int TagMetric(int tag)
{
	switch (tag)
	{
	case ST_AIJOIN:    return M_ML_AIJOIN;
	case ST_PATH:      return M_ML_PATH;
	case ST_KILL:      return M_ML_KILL;
	case ST_CHARSUT:   return M_ML_CHARSUT;
	case ST_SAVE:      return M_ML_SAVE;
	case ST_RAGDOLL:   return M_ML_RAGDOLL;
	case ST_PHYSKICK:  return M_ML_PHYSKICK;
	case ST_ZONECAM:   return M_ML_ZONECAM;
	case ST_PLAYER:    return M_ML_PLAYER;
	case ST_MISCA:     return M_ML_MISCA;
	case ST_PARTICLES: return M_ML_PARTICLES;
	case ST_MISCB:     return M_ML_MISCB;
	case ST_ZONEMT:    return M_ML_ZONEMT;
	case ST_FACTORY:   return M_ML_FACTORY;
	case ST_BIRDSJOIN: return M_ML_BIRDSJOIN;
	case ST_CHARS:     return M_ML_CHARS;
	case ST_CHARSP:    return M_ML_CHARS;
	case ST_FACTIONS:  return M_ML_FACTIONS;
	case ST_BIRDSKICK: return M_ML_BIRDSKICK;
	case ST_GUI:       return M_ML_GUI;
	default:           return -1;   // ST_AIKICK runs inside frameStarted
	}
}

// ---- Cursor ray (main thread only: the `mouseRay` probe's enter / exit) ----

// Change-class thresholds against the previous ray: origin distance (world
// units) and the chord between unit directions (|a - b| = 2 sin(angle / 2)).
const float CUR_STILL_ORIGIN = 0.1f;
const float CUR_SMALL_ORIGIN = 2.0f;
const float CUR_STILL_CHORD2 = 7.6154e-7f;   // 0.05 degrees, squared chord
const float CUR_SMALL_CHORD2 = 7.6154e-5f;   // 0.5 degrees

// Stack pointers to float vectors: 4-byte aligned, not necessarily 8.
inline bool PlausibleVec(uintptr_t p)
{
	return p >= 0x10000 && p < 0x00007FFFFFFFFFF0ULL && (p & 3) == 0;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

unsigned PhysicsRunning()
{
	if (!g_cursor.gw)
		return 0;
	uintptr_t th = *(const uintptr_t*)(KLIB_MEMBER(5, g_cursor.gw, GameWorld_physics, GW_PHYSICS));
	if (!PlausiblePtr(th))
		return 0;
	return *(const unsigned char*)(KLIB_MEMBER(5, th, ThreadClass__running, THREAD_RUNNING)) ? 1u : 0u;
}

} // audit


namespace kenshiframeaudit_detail {

inline bool IsCharGroup(int g)
{
	return g >= 0 && g < 32 && ((1u << g) & g_cfg.cursorCharGroups) != 0;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
IsIndoors_t oIsIndoors = NULL;

void* hk_IsIndoors(const void* point)
{
	bool sample = g_cfg.physxDetail && IsMain() && g_cur.open && g_cur.mouseScanDepth > 0;
	if (!sample)
		return oIsIndoors(point);
	PhysQueryEnter(PQ_INDOORS, CurrentNpScene());
	LONGLONG t0 = Now();
	void* result = oIsIndoors(point);
	LONGLONG t1 = Now();
	PhysQueryExit(PQ_INDOORS, t1 - t0);
	return result;
}

void CursorEnter(CallSiteProbe::U64 result, CallSiteProbe::U64 origin, CallSiteProbe::U64 dir,
                 CallSiteProbe::U64 group)
{
	CursorState& s = g_cursor;
	CurFrame& c = g_cur;
	s.inCall = true;
	s.split  = false;
	s.result = (uintptr_t)result;
	s.group  = (unsigned)group;
	s.sortAtEnter = c.sub[SUBT_CURSORT];
	s.rayOk  = PlausibleVec((uintptr_t)origin) && PlausibleVec((uintptr_t)dir);

	int cls = CUR_LARGE;
	if (s.rayOk)
	{
		const float* o = (const float*)origin;
		const float* d = (const float*)dir;
		float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		float inv = len != 0.0f ? 1.0f / len : 1.0f;
		for (int i = 0; i < 3; ++i)
		{
			s.o[i] = o[i];
			s.d[i] = d[i] * inv;
		}
		if (s.haveLast)
		{
			float o2 = 0.0f, d2 = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				float a = s.o[i] - s.lastO[i];
				float b = s.d[i] - s.lastD[i];
				o2 += a * a;
				d2 += b * b;
			}
			if (o2 <= CUR_STILL_ORIGIN * CUR_STILL_ORIGIN && d2 <= CUR_STILL_CHORD2)
				cls = CUR_STILL;
			else if (o2 <= CUR_SMALL_ORIGIN * CUR_SMALL_ORIGIN && d2 <= CUR_SMALL_CHORD2)
				cls = CUR_SMALL;
		}
		for (int i = 0; i < 3; ++i)
		{
			s.lastO[i] = s.o[i];
			s.lastD[i] = s.d[i];
		}
		s.haveLast = true;
	}

	unsigned bits = 0;
	if (s.key)
	{
		const unsigned char* k = (const unsigned char*)s.key;
		const unsigned char* m = k + IH_MLEFT;   // mLeft .. mRUp, 8 bools
		for (int i = 0; i < 8; ++i)
			if (m[i]) bits |= 1u << i;           // CI_MLEFT .. CI_MRUP
		if (k[IH_CTRL])     bits |= CI_CTRL;
		if (k[IH_CTRL + 1]) bits |= CI_SHIFT;
		if (k[IH_CTRL + 2]) bits |= CI_ALT;
		const float* pos = (const float*)(k + IH_MPOS);
		if (s.havePos && (pos[0] != s.lastPos[0] || pos[1] != s.lastPos[1]))
			bits |= CI_MOVED;
		s.lastPos[0] = pos[0];
		s.lastPos[1] = pos[1];
		s.havePos = true;
	}
	if (s.prevMLeft && *(const unsigned char*)s.prevMLeft)   bits |= CI_PREVMLEFT;
	if (s.prevMRight && *(const unsigned char*)s.prevMRight) bits |= CI_PREVMRIGHT;
	s.physStart = PhysicsRunning();
	if (s.physStart) bits |= CI_PHYS;

	bool held = (bits & (CI_MLEFT | CI_MRIGHT)) != 0;
	bool edge = (bits & (CI_MLDOWN | CI_MRDOWN | CI_MLUP | CI_MRUP)) != 0 ||
	            ((bits & CI_MLEFT) != 0) != ((bits & CI_PREVMLEFT) != 0) ||
	            ((bits & CI_MRIGHT) != 0) != ((bits & CI_PREVMRIGHT) != 0);

	++c.curCalls;
	++c.curClass[cls];
	if (bits & CI_MOVED) ++c.curMoved;
	if (held)            ++c.curBtn;
	if (edge)            ++c.curEdge;
	if (bits & (CI_CTRL | CI_SHIFT | CI_ALT)) ++c.curMod;
	if (s.physStart)     ++c.curPhys;
	c.curInput |= bits;

	if (s.splitOn && s.rayOk && !c.curSplit)
	{
		++s.rays;
		s.split = (s.rays % (unsigned long long)g_cfg.cursorSplitEvery) == 0;
	}
}

// PhysX 2.8 NxUserRaycastReport, laid out like the game's HitCallback (vtable
// 0x1741BF0): onHit in slot 0, the virtual destructor in slot 1. PhysX calls
// only onHit. Counts hits and character-group hits; never stops the query.
// Runs inside the raycast, under the scene's query lock.
class CursorCountReport
{
public:
	CursorCountReport() : hits(0), chars(0) {}
	virtual bool onHit(const void* hit)
	{
		++hits;
		uintptr_t shape = *(const uintptr_t*)hit;          // NxRaycastHit::shape
		if (PlausiblePtr(shape))
		{
			uintptr_t ud = *(const uintptr_t*)(shape + SHAPE_USERDATA);
			if (PlausiblePtr(ud) && IsCharGroup(*(const int*)(ud + USERDATA_GROUP)))
				++chars;
		}
		return true;
	}
	virtual ~CursorCountReport() {}
	int hits, chars;
};

typedef unsigned (*RaycastAll_t)(void* scene, const float* ray, void* report, int shapesType,
                                 unsigned groups, float maxDist, unsigned hintFlags, const void* groupsMask);

// The scene's raycastAllShapes, resolved the way traceAll does it and checked
// to lie in PhysXCore64.dll (looked up by OncePerSecond, outside any probe).
// NULL while there is no scene or no module handle yet; NULL, with the split
// switched off for the session, when the function is anywhere else.
RaycastAll_t CursorRaycastFn(void** scene)
{
	CursorState& s = g_cursor;
	if (!s.physxCore)
		return NULL;
	uintptr_t phys = *(const uintptr_t*)(KLIB_MEMBER(5, s.gw, GameWorld_physics, GW_PHYSICS));
	uintptr_t sc   = PlausiblePtr(phys) ? *(const uintptr_t*)(phys + PHYS_NWORLD) : 0;
	uintptr_t vt   = PlausiblePtr(sc) ? *(const uintptr_t*)sc : 0;
	void*     fn   = PlausiblePtr(vt) ? *(void* const*)(vt + NXSCENE_RAYCAST_ALL) : NULL;
	if (!fn)
		return NULL;
	uintptr_t mb = 0, me = 0;
	if (!ModuleRange(fn, &mb, &me) || mb != (uintptr_t)s.physxCore)
	{
		s.splitOn     = false;   // reported by OncePerSecond
		s.splitOffWhy = "NxScene vt+0x370 is not in PhysXCore64.dll";
		return NULL;
	}
	*scene = (void*)sc;
	return (RaycastAll_t)fn;
}

// On a split frame, right after the game's own call: the same ray through the
// same scene API, five ways. Main thread, inside mouseTraceAll.
void CursorSplitRays(LONGLONG realTicks, int realHits, int realChars)
{
	CursorState& s = g_cursor;
	void* scene = NULL;
	RaycastAll_t fn = CursorRaycastFn(&scene);
	if (!fn)
		return;

	CursorSplit& sp = g_cur.split;
	sp.ms[SR_REAL]    = TicksToMs(realTicks);
	sp.hits[SR_REAL]  = (unsigned short)(realHits > 0xFFFF ? 0xFFFF : realHits);
	sp.chars[SR_REAL] = (unsigned short)(realChars > 0xFFFF ? 0xFFFF : realChars);
	sp.realSortMs     = g_haveTag[ST_CURSORT] ? TicksToMs(g_cur.sub[SUBT_CURSORT] - s.sortAtEnter) : Nan();
	sp.physAtStart    = (unsigned char)s.physStart;

	float ray[6] = { s.o[0], s.o[1], s.o[2], s.d[0], s.d[1], s.d[2] };   // NxRay: orig, dir
	// shapesType: 1 = static, 2 = dynamic, 3 = both (NxShapesType).
	const int      shapes[NUM_SPLITRAYS] = { 0, 3, 1, 2, 2, 2 };
	const unsigned groups[NUM_SPLITRAYS] = { 0, s.group, s.group, s.group, s.group, s.group & ~g_cfg.cursorCharGroups };
	for (int i = SR_FULL; i < NUM_SPLITRAYS; ++i)
	{
		CursorCountReport rep;
		LONGLONG a = Now();
		fn(scene, ray, &rep, shapes[i], groups[i], FLT_MAX, TRACE_HINT_FLAGS, NULL);
		LONGLONG b = Now();
		sp.ms[i]    = TicksToMs(b - a);
		sp.hits[i]  = (unsigned short)(rep.hits > 0xFFFF ? 0xFFFF : rep.hits);
		sp.chars[i] = (unsigned short)(rep.chars > 0xFFFF ? 0xFFFF : rep.chars);
	}
	sp.physAtEnd   = (unsigned char)PhysicsRunning();
	g_cur.curSplit = true;
}

void CursorExit(LONGLONG ticks)
{
	CursorState& s = g_cursor;
	if (!s.inCall)
		return;
	s.inCall = false;

	// The hit list the game's HitCallback filled, already sorted by the qsort
	// inside traceAll; mouseScan reads it after this returns.
	int hits = 0, chars = 0;
	if (s.layoutOk && PlausiblePtr(s.result))
	{
		unsigned n = *(const unsigned*)(s.result + HIT_LIST_COUNT);
		uintptr_t data = *(const uintptr_t*)(s.result + HIT_LIST_DATA);
		if (n > 0 && n <= 65535 && PlausiblePtr(data))
		{
			CurFrame& c = g_cur;
			for (unsigned i = 0; i < n; ++i)
			{
				int g = *(const int*)(data + (uintptr_t)i * HIT_STRIDE + HIT_GROUP);
				int bin = (g >= 0 && g < 32) ? g : 32;
				if (c.curGroups[bin] != 0xFFFF)
					++c.curGroups[bin];
				if (IsCharGroup(g))
					++chars;
			}
			hits = (int)n;
			c.curHits += hits;
			c.curChar += chars;
		}
	}
	if (s.split)
		CursorSplitRays(ticks, hits, chars);
}

using CallSiteProbe::SHAPE_INT;
using CallSiteProbe::SHAPE_FLOAT;
using CallSiteProbe::SHAPE_FLOAT4;
using CallSiteProbe::SHAPE_RETFLOAT;

// Site RVA -> the thunk its E8 must decode to (verified in IDA 2026-09-13).
// InitKlibBindings already compared covered callee implementations; retain
// these raw thunk RVAs because the interior E8 still encodes the thunk.
CallSiteProbe::Site g_sites[] =
{
	// frameStarted
	{ "aiKick",     0x82A9F3, 0x49071, SHAPE_FLOAT, ST_AIKICK },
	// GameWorld::mainLoop_GPUSensitiveStuff
	{ "aiJoin",     0x787ECA, 0x4F8AE, SHAPE_INT,   ST_AIJOIN },
	{ "path",       0x787EDB, 0x31372, SHAPE_INT,   ST_PATH },
	{ "kill",       0x788020, 0x489F5, SHAPE_INT,   ST_KILL },
	{ "charsUT",    0x78803B, 0x28D5D, SHAPE_INT,   ST_CHARSUT },
	{ "save",       0x7880B1, 0x17607, SHAPE_INT,   ST_SAVE },
	{ "ragdoll",    0x7880B9, 0x54089, SHAPE_INT,   ST_RAGDOLL },
	{ "physKick",   0x78816B, 0x49071, SHAPE_FLOAT, ST_PHYSKICK },
	{ "zoneCam",    0x7881FA, 0x46FC9, SHAPE_INT,   ST_ZONECAM },
	{ "player",     0x78820B, 0x384FB, SHAPE_INT,   ST_PLAYER },
	{ "miscA",      0x78825C, 0x18FB1, SHAPE_INT,   ST_MISCA },
	{ "particles",  0x7882AD, 0x0DDC3, SHAPE_INT,   ST_PARTICLES },
	{ "miscB",      0x7882FE, 0x2A810, SHAPE_INT,   ST_MISCB },
	{ "zoneMT",     0x788414, 0x0FA1A, SHAPE_INT,   ST_ZONEMT },
	{ "factory",    0x788420, 0x18CD7, SHAPE_INT,   ST_FACTORY },
	{ "birdsJoin",  0x78844D, 0x4F8AE, SHAPE_INT,   ST_BIRDSJOIN },
	{ "chars",      0x788467, 0x4B628, SHAPE_INT,   ST_CHARS },
	{ "charsPaused",0x788476, 0x0B4C4, SHAPE_INT,   ST_CHARSP },
	{ "factions",   0x788485, 0x234A7, SHAPE_FLOAT, ST_FACTIONS },
	{ "birdsKick",  0x7884A9, 0x49071, SHAPE_FLOAT, ST_BIRDSKICK },
	{ "gui",        0x7884C2, 0x2670B, SHAPE_INT,   ST_GUI },
	// Nested in `player`: PlayerInterface's mouse scan (0x7FF6B0), its
	// cursor raycast (0x9B28D0, raycastAll + qsort), the camera floor ray.
	{ "mouseScan",  0x800831, 0x3BDAE, SHAPE_INT,   ST_MOUSESCAN },
	{ "mouseRay",   0x9B2AE2, 0x1C79C, SHAPE_INT,   ST_MOUSERAY },
	{ "camRay",     0x80102E, 0x2E2C1, SHAPE_INT,   ST_CAMRAY },
	// mouseScan's level-editor click ray (UtilityT::rayTrace 0x9B6490 via its
	// thunk), taken only with PlayerInterface+0x298 set: 0 calls in normal play.
	{ "mouseRay2",  0x7FF78A, 0x1AF28, SHAPE_INT,   ST_MOUSERAY2 },
	// traceAll's `call [__imp_qsort]` (FF 15, indirect): the cursor hit sort.
	{ "cursorSort", 0x9B2A09, 0x2244B88, SHAPE_INT, ST_CURSORT, 0, 1 },
	// Nested in `zoneCam`: vanilla updateCameraZone's (0xA11DA0) own calls.
	{ "zcContent",  0xA11F28, 0x05FB0, SHAPE_INT,   ST_ZC_CONTENT },  // FoliageSystem::update, per Set B zone with foliage
	{ "zcMisc",     0xA11F3D, 0x08F2B, SHAPE_INT,   ST_ZC_MISC },     // sub_14040BA60(ZM+0x1682A8)
	{ "zcAct",      0xA12033, 0x2C47B, SHAPE_INT,   ST_ZC_ACT },      // activateZone
	{ "zcDeact",    0xA12072, 0x4848C, SHAPE_INT,   ST_ZC_DEACT },    // deactivateZone
	{ "zcSect",     0xA1208A, 0x193D5, SHAPE_INT,   ST_ZC_SECT },     // SectionManager::perFrameUpdate
	{ "zcMaint",    0xA120C6, 0x431DF, SHAPE_INT,   ST_ZC_MAINT },    // SectionManager::maintenanceTimer (2 s)
	// DeferredLightingPass::execute (0x2D9500) -> light renderer 0x2D8500 (lights + CSM).
	{ "lights",     0x2D953D, 0x0A911, SHAPE_INT,   ST_LIGHTS },
	// Nested in `chars` (SteadyDetail): GameWorld::charsUpdate's (0x7862F0) per-character calls.
	// periodicUpdate's site loads rax from [rdi] and rcx from rdi: the same object, hence looseVirtual=1.
	{ "chPeriodic", 0x7863FF, 0,       SHAPE_INT,   ST_CH_PERIODIC, 0xE8, 0, 1 },  // Character::periodicUpdate, at most 8 a frame
	{ "chFour",     0x786563, 0x48CD4, SHAPE_INT,   ST_CH_FOUR },     // Character::fourFrameUpdate, visible characters
	{ "chFourOff",  0x7864FC, 0x48CD4, SHAPE_INT,   ST_CH_FOUR },     // ... the off-screen ones (at most 6 a frame)
	{ "chPost",     0x7866A7, 0,       SHAPE_INT,   ST_CH_POST, 0x268 },          // Character::postUpdate, every character
	{ "chRemoval",  0x78663A, 0x158CF, SHAPE_INT,   ST_CH_DEATH },    // GameWorld::processUpdateRemovalList
	{ "chDeath",    0x786642, 0x1D6B0, SHAPE_INT,   ST_CH_DEATH },    // GameWorld::charsUpdateDeathParade
	// Nested in `factions` (SteadyDetail): FactionManager::updateMT (0x2E74B0) and Faction::update (0x6BA9B0).
	{ "fcUpdate",   0x2E74EB, 0x262A6, SHAPE_FLOAT, ST_FC_UPDATE },   // Faction::update, every faction
	{ "fcActive",   0x6BA9E8, 0x3F9D1, SHAPE_FLOAT, ST_FC_ACTIVE },   // Faction::updateActivePlatoons
	{ "fcPeriodic", 0x2E750F, 0x0937C, SHAPE_INT,   ST_FC_PERIODIC }, // Faction::periodicUpdateMT, one faction a frame
	// Nested in chPeriodic (SteadyDetail): Character::periodicUpdate's (0x5CC300) light-level call,
	// GameWorld::getLightLevel 0xA0A040 (a float result), and three calls inside it that run only
	// at night. Its two 0xA07ED0 calls take a stack argument: counted from the list counts instead.
	{ "chLight",    0x5CC7C4, 0x1F488, SHAPE_RETFLOAT, ST_CH_LIGHT },  // getLightLevel
	{ "lzZones",    0xA0A128, 0x311BA, SHAPE_FLOAT4,   ST_LZ_ZONES },  // findOverlappingActiveZones (xmm3 = radius)
	{ "lzBld",      0xA0A230, 0x3482E, SHAPE_FLOAT4,   ST_LZ_BLD },    // Building::getLights, every object walked
	{ "lzChar",     0xA0A356, 0x0B5E6, SHAPE_INT,      ST_LZ_CHAR },   // AppearanceBase::getLights, every lit character
	// RenderTimeBackthread body (AI thread)
	{ "aiZone",     0x786E41, 0x202CF, SHAPE_INT,   ST_AIZONE },
	{ "aiContent",  0x786EF0, 0x455A2, SHAPE_INT,   ST_AICONTENT },
	{ "aiFactions", 0x786F04, 0x02464, SHAPE_FLOAT, ST_AIFACTIONS },
	{ "aiVis",      0x786FB9, 0x3560C, SHAPE_INT,   ST_AIVIS1 },
	{ "aiVisDead",  0x787004, 0x3560C, SHAPE_INT,   ST_AIVIS2 },
	{ "aiWeather",  0x786E95, 0x33A73, SHAPE_INT,   ST_AIENV },       // weather / environment blender
	{ "aiSmell",    0x786EE4, 0x0365C, SHAPE_FLOAT, ST_AIENV },       // zone smells (xmm1 = time)
	{ "aiTimer",    0x786F4F, 0x2B9B8, SHAPE_INT,   ST_AIENV },       // "10 s in one camera section"
	{ "aiForced1",  0x786F65, 0x30FA8, SHAPE_INT,   ST_AIFORCED },    // forced periodic update (deque +0x170)
	{ "aiForced2",  0x786F7B, 0x30FA8, SHAPE_INT,   ST_AIFORCED },
	// The three per-character list loops: `call [rax+slot]` (virtual sites).
	{ "aiTU",       0x787080, 0,       SHAPE_INT,   ST_AITU,  0xD8 },
	{ "aiTU4",      0x7870AE, 0,       SHAPE_INT,   ST_AITU4, 0x278 },
	{ "aiTUP",      0x7870DE, 0,       SHAPE_INT,   ST_AITUP, 0x280 },
	// Inside threadedUpdate 0x5C71A0 (list 1): AI task-system flush, animation.
	{ "aiFlush",    0x5C7260, 0x0F65F, SHAPE_INT,   ST_AIFLUSH },
	{ "aiAnim",     0x5C7274, 0x207B1, SHAPE_FLOAT, ST_AIANIM },      // AnimationClass::update (xmm1 = dt)
	// Inside Faction::periodicUpdateThreaded (0x6B9580, every faction, every AI run, SteadyDetail):
	// one active platoon's periodicUpdate_unloaded (Platoon vtable +0xE0).
	{ "afPlatoonU", 0x6B95F7, 0,       SHAPE_INT,   ST_AF_PLATOONU, 0xE0 },
	// PhysicsActual body (physics thread)
	{ "physLock",   0x7DC514, 0x25FCC, SHAPE_INT,   ST_PHYSLOCK },    // timed_lock(+0x98) before the step
	{ "physPre",    0x7DC536, 0x0A78B, SHAPE_INT,   ST_PHYSPRE },     // threaded-object pre-step (0x4CBB90)
	{ "physPost",   0x7DC5F6, 0x17A58, SHAPE_INT,   ST_PHYSPOST },    // post-step queue flush (0x4CCEB0)
	// PhysicsActual::threadJunkPreBT direct calls. Creation and hull-application
	// virtual calls use short FF 50 encodings and are timed by entry hooks.
	{ "physGroup",      0x4CBDDD, 0x4EE5E, SHAPE_INT, ST_PHYS_GROUP },
	{ "physActorDestroy",0x4CBFDE,0x36CA5, SHAPE_INT, ST_PHYS_ACTOR_DESTROY },
	{ "physTerrain",    0x4CC00A, 0x38CE9, SHAPE_INT, ST_PHYS_TERRAIN },
	// The indirect simulate call is the gap between physPre exit and physFlush
	// entry. Fetch has argument setup after mov rax,[rcx], hence looseVirtual=1.
	{ "physFlush",      0x7DC568, 0,       SHAPE_INT, ST_PHYS_FLUSH, 0x288 },
	{ "physFetch",      0x7DC582, 0,       SHAPE_INT, ST_PHYS_FETCH, 0x478, 0, 1 }
};
extern const int NUM_SITES = sizeof(g_sites) / sizeof(g_sites[0]);

// A lektor's element count (+8, as the hit list); 0 when the pointer or the count is implausible.
static int LektorCount(uintptr_t lektor)
{
	if (!PlausiblePtr(lektor))
		return 0;
	unsigned n = *(const unsigned*)(lektor + HIT_LIST_COUNT);
	return n <= 65535 ? (int)n : 0;
}

void OnProbeEnter(int id, CallSiteProbe::U64 a, CallSiteProbe::U64 b, CallSiteProbe::U64 c,
                  CallSiteProbe::U64 d)
{
	int tag = CallSiteProbe::TagOf(id);
	if (tag >= ST_PHYS_FIRST)
	{
		if (!IsMain())
			PhysProbeEnter(tag, (uintptr_t)a);
		return;
	}
	if (tag >= ST_AI_FIRST || !IsMain() || !g_cur.open)
		return;
	++g_cur.probeDepth;
	switch (tag)
	{
	case ST_AIKICK:    InterlockedExchange(&g_ai.kickSeq, (LONG)g_cur.seq); break;
	case ST_PHYSKICK:  InterlockedExchange(&g_phys.kickSeq, (LONG)g_cur.seq); break;
	case ST_BIRDSKICK: InterlockedExchange(&g_birds.kickSeq, (LONG)g_cur.seq); break;
	case ST_PATH:      CollectAi(); break;
	case ST_KILL:      g_cur.flags |= F_PHYSRAN; CollectPhys(); break;
	case ST_CHARS:
	case ST_CHARSP:    CollectBirds(); break;
	case ST_PARTICLES: if (g_fxCensusOn) FxCensus(); break;   // before the particle job
	case ST_MOUSERAY:
		PhysQueryEnter(PQ_MOUSE_ALL, 0);
		CursorEnter(a, b, c, d);
		break;                                                   // traceAll(result, origin, dir, group)
	case ST_MOUSESCAN: ++g_cur.mouseScanDepth; break;
	case ST_MOUSERAY2: ++g_cur.ray2Calls; break;
	case ST_LZ_ZONES: g_cur.lzOut = (uintptr_t)b; break;        // (zm, out, pos, radius)
	case ST_LZ_BLD:   g_cur.lzList = (uintptr_t)b; break;       // (building, list, pos, radius squared)
	case ST_LZ_CHAR:  g_cur.lzCharList = (uintptr_t)b; break;   // (appearance, list)
	case ST_ZONECAM:
		if (b >= 0x10000 && b < 0x00007FFFFFFFFFFFULL)
		{
			const float* p = (const float*)b;
			g_cur.cam[0] = *(const float*)KLIB_MEMBER(5, p, Ogre__Vector3_x, 0);
			g_cur.cam[1] = *(const float*)KLIB_MEMBER(5, p, Ogre__Vector3_y, 4);
			g_cur.cam[2] = *(const float*)KLIB_MEMBER(5, p, Ogre__Vector3_z, 8);
			g_cur.camValid = true;
		}
		break;
	default:
		break;
	}
}

void OnProbeExit(int id, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1)
{
	int tag = CallSiteProbe::TagOf(id);
	LONGLONG d = t1 - t0;

	if (tag >= ST_PHYS_FIRST)
	{
		if (IsMain())
			return;          // physics body run inline: not split
		PhysProbeExit(tag, t1);
		switch (tag)
		{
		case ST_PHYSLOCK: g_phys.lock += d; break;
		case ST_PHYSPRE:  g_phys.pre  += d; break;
		default:          g_phys.post += d; break;
		}
		return;
	}
	if (tag >= ST_AI_FIRST)
	{
		if (IsMain())
			return;          // inline AI body (characterMultithreading off): not split
		switch (tag)
		{
		case ST_AIZONE:     g_ai.zone += d; break;
		case ST_AICONTENT:  g_ai.content += d; break;
		case ST_AIFACTIONS: g_ai.factions += d; break;
		case ST_AITU:
			g_ai.tu += d;
			if (d > g_ai.tuMax) g_ai.tuMax = d;
			break;
		case ST_AITU4:      g_ai.tu4 += d; break;
		case ST_AITUP:      g_ai.tup += d; break;
		case ST_AIENV:      g_ai.env += d; break;
		case ST_AIFORCED:   g_ai.forced += d; break;
		case ST_AIFLUSH:    g_ai.l1Flush += d; break;
		case ST_AIANIM:     g_ai.l1Anim += d; break;
		case ST_AF_PLATOONU: g_ai.platU += d; break;
		default:           g_ai.vis += d; ++g_ai.visCalls; break;
		}
		return;
	}

	if (!IsMain() || !g_cur.open)
		return;
	--g_cur.probeDepth;
	int metric = TagMetric(tag);
	if (metric >= 0)
		g_cur.ml[metric - ML_FIRST] += d;
	switch (tag)
	{
	case ST_MOUSESCAN:
		g_cur.sub[SUBT_MOUSESCAN] += d;
		if (g_cur.mouseScanDepth > 0) --g_cur.mouseScanDepth;
		break;
	case ST_MOUSERAY:
		g_cur.sub[SUBT_MOUSERAY] += d;
		PhysQueryExit(PQ_MOUSE_ALL, d);
		CursorExit(d);
		break;
	case ST_CAMRAY:     g_cur.sub[SUBT_CAMRAY]    += d; break;
	case ST_MOUSERAY2:  g_cur.sub[SUBT_MOUSERAY2] += d; break;
	case ST_CURSORT:    g_cur.sub[SUBT_CURSORT]   += d; break;
	case ST_ZC_CONTENT: g_cur.sub[SUBT_ZCCONTENT] += d; break;
	case ST_ZC_MISC:    g_cur.sub[SUBT_ZCMISC]    += d; break;
	case ST_ZC_ACT:
	case ST_ZC_DEACT:   g_cur.sub[SUBT_ZCACT]     += d; break;
	case ST_ZC_SECT:
	case ST_ZC_MAINT:   g_cur.sub[SUBT_ZCSECT]    += d; break;
	case ST_LIGHTS:     g_cur.sub[SUBT_LIGHTS]    += d; break;
	case ST_CH_PERIODIC: g_cur.sd[SDT_CHPERIODIC] += d; ++g_cur.chPeriodicN; break;
	case ST_CH_FOUR:     g_cur.sd[SDT_CHFOUR]     += d; break;
	case ST_CH_POST:     g_cur.sd[SDT_CHPOST]     += d; break;
	case ST_CH_DEATH:    g_cur.sd[SDT_CHDEATH]    += d; break;
	case ST_FC_UPDATE:   g_cur.sd[SDT_FCUPDATE]   += d; break;
	case ST_FC_ACTIVE:   g_cur.sd[SDT_FCACTIVE]   += d; break;
	case ST_FC_PERIODIC: g_cur.sd[SDT_FCPERIODIC] += d; break;
	case ST_CH_LIGHT:    g_cur.sd[SDT_CHLIGHT] += d; ++g_cur.chLightN; break;
	case ST_LZ_ZONES:
		g_cur.sd[SDT_LZZONES] += d;
		++g_cur.lzCalls;
		g_cur.lzZoneN += LektorCount(g_cur.lzOut);
		break;
	case ST_LZ_BLD:
		g_cur.sd[SDT_LZBLD] += d;
		++g_cur.lzBldN;
		if ((unsigned)ret != 0)   // the caller walks the list only on a non-zero return
			g_cur.lzLightN += LektorCount(g_cur.lzList);
		break;
	case ST_LZ_CHAR:
		g_cur.sd[SDT_LZCHAR] += d;
		++g_cur.lzCharN;
		g_cur.lzCharLightN += LektorCount(g_cur.lzCharList);
		break;
	case ST_AIKICK:    g_cur.aiKickT = t0; break;
	case ST_PHYSKICK:  g_phys.kickT  = t0; break;
	case ST_BIRDSKICK: g_birds.kickT = t0; break;
	case ST_AIJOIN:
		g_cur.flags   |= F_AIJOIN;
		g_cur.aiJoinT0 = t0;
		g_cur.aiJoinT1 = t1;
		break;
	case ST_BIRDSJOIN:
		g_cur.flags      |= F_BIRDSJOIN;
		g_cur.birdsJoinT0 = t0;
		break;
	default:
		break;
	}
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
