// audit_offmain.cpp - The off-main probes' exe half (OffMainDetail=1). On the main thread: the
// particle job's fork/join, the time before it and the drain of finished effects
// (EffectsManager::update), foliage page loads by caller, tree and grass page generation, the batch
// build, the sub-batch builds (wind ones apart), the merge's fork/join and the buffer locks, and the
// cursor scan's own calls with isIndoors outside the scan. On Ogre's workers: the particle job's
// execute. No hook or callback takes a lock, allocates, or logs; the worker parts are Interlocked
// only; the [AUDIT-SYNC] line is written from the main thread's once-a-second pass.

#include "audit_offmain.h"
#include "audit_scene.h"
#include <limits.h>

namespace audit_offmain_detail {

// Main-thread totals of the frame in flight: OffMainFrameTotals writes them out and clears them.
struct OffMainFrame
{
	LONGLONG fxFork, fxPre, fxDrain, fxJobMax, fxJobSum, fxWake, fxTail;
	LONGLONG fpLoad, fpLoadCache, fpTree, fpGrass, fpBuild, fpSub, fpSubWind, fpPoolWait, fpLockRO, fpLockDst;
	LONGLONG msTrace, msIndoors, msNavValid, msIndoorsFast, msTerrain, indoorsOther;
	int      fxDrainN, fpLoadN, fpLoadCacheN, fpSubN, fpSubWindN, fpLockN, indoorsOtherN, indoorsOtherPhys;
};

} // audit_offmain_detail
using namespace audit_offmain_detail;

namespace kenshiframeaudit_detail {

using CallSiteProbe::SHAPE_INT;

// WindBatchedGeometry::WindSubBatch's vtable: a sub-batch whose build is the wind one.
static const size_t WIND_SUBBATCH_VT = 0x174F908;

// Entry detours (Steam 1.0.65).
static const size_t RVA_FX_JOB       = 0x40ECD0;  // EffectsManagerUpdateJob, vtable slot 0 (Ogre workers)
static const size_t RVA_FP_BUILD     = 0xA56400;  // BatchedGeometry::build
static const size_t RVA_FP_TREE_LOAD = 0xA46F40;  // TreeLoader3D::loadPage
static const size_t RVA_FP_GRASS     = 0xA3A7E0;  // GrassLoader::loadPage

static const unsigned char PRO_FX_JOB[16]       = { 0x48,0x89,0x6C,0x24,0x20,0x57,0x41,0x54,0x41,0x55,0x48,0x83,0xEC,0x20,0x48,0x8B };
static const unsigned char PRO_FP_BUILD[16]     = { 0x48,0x89,0x4C,0x24,0x08,0x56,0x57,0x48,0x81,0xEC,0x18,0x0E,0x00,0x00,0x48,0xC7 };
static const unsigned char PRO_FP_TREE_LOAD[16] = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x56,0x57,0x48,0x81,0xEC,0x58 };
static const unsigned char PRO_FP_GRASS[16]     = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x48,0x81,0xEC,0x58,0x02,0x00 };

typedef unsigned __int64 (*FxJob_t)(void*, int, int);
typedef unsigned __int64 (*FpBuild_t)(void*);
typedef void             (*FpLoadPage_t)(void*, void*);

static FxJob_t      oFxJob      = NULL;
static FpBuild_t    oFpBuild    = NULL;
static FpLoadPage_t oFpTreeLoad = NULL;
static FpLoadPage_t oFpGrass    = NULL;
static bool s_fxJobHooked, s_fpBuildHooked, s_fpTreeHooked, s_fpGrassHooked;

// Call sites (Steam 1.0.65): an E8 must decode to the function or thunk named, an FF 15 to the
// import slot named.
static CallSiteProbe::Site s_exeSites[] =
{
	// EffectsManager::update 0x40A4E0: the particle job's fork/join, then one call per finished effect.
	{ "fxFork",        0x40A615, 0x2247DD8, SHAPE_INT, ST_FX_FORK, 0, 1 },      // SceneManager::executeUserScalableTask, blocking
	{ "fxDrain",       0x40A64B, 0x040C5,   SHAPE_INT, ST_FX_DRAIN },           // one finished effect
	// PagedGeometry's _loadPage 0xA2D700, by caller.
	{ "fpLoadNear",    0xA2B8FF, 0xA2D700,  SHAPE_INT, ST_FP_LOAD },            // GeometryPageManager::update: a page in the visible band
	{ "fpLoadCache",   0xA2BDA1, 0xA2D700,  SHAPE_INT, ST_FP_LOADCACHE },       // ... its pending queue, one page per cache interval
	{ "fpLoadArea",    0xA2D56E, 0xA2D700,  SHAPE_INT, ST_FP_LOAD },            // every unloaded page of an area
	// BatchedGeometry::build's sub-batch loop: rax and rcx are both loaded from the same object.
	{ "fpSub",         0xA5675F, 0,         SHAPE_INT, ST_FP_SUB, 0x90, 0, 1 }, // SubBatch::build, the wind sub-batch's included
	{ "fpPoolWait",    0xA587EA, 0x2247DD8, SHAPE_INT, ST_FP_POOLWAIT, 0, 1 },  // the non-wind merge's fork/join
	// HardwareBuffer::lock(LockOptions, UploadOptions) in the non-wind build 0xA57A60.
	{ "fpLockN1",      0xA57C20, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockN2",      0xA57E11, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockN3",      0xA58095, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockN4",      0xA5859C, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockN5",      0xA58632, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	// ... and in the wind build 0xA67710.
	{ "fpLockW1",      0xA6788C, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockW2",      0xA678B6, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockW3",      0xA67CC1, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockW4",      0xA67F21, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	{ "fpLockW5",      0xA68453, 0x2246148, SHAPE_INT, ST_FP_LOCK, 0, 1 },
	// PlayerInterface::mouseScan 0x7FF6B0's own calls.
	{ "msTrace",       0x7FF7ED, 0x3C8B2,   SHAPE_INT, ST_MS_TRACE },           // UtilityT::mouseTraceAll
	{ "msIndoors",     0x7FF8C3, 0x43D47,   SHAPE_INT, ST_MS_INDOORS },         // UtilityT::isIndoors
	{ "msNavValid",    0x7FFFA5, 0x24802,   SHAPE_INT, ST_MS_NAVVALID },        // NavMesh::getPositionValid
	{ "msIndoorsFast", 0x80017B, 0x21D05,   SHAPE_INT, ST_MS_INDOORSFAST },     // UtilityT::isIndoorsFast, the right click
	{ "msTerrain",     0x7FFFFC, 0x2248F10, SHAPE_INT, ST_MS_TERRAIN, 0, 1 },   // Terrain::intersect (its hit returned through rdx)
};
static const int NUM_EXE_SITES = sizeof(s_exeSites) / sizeof(s_exeSites[0]);
static const int NUM_HOOKS     = 4;
static const int MAX_ROW_SET   = 32;
static_assert(sizeof(s_exeSites) / sizeof(s_exeSites[0]) <= MAX_ROW_SET, "one Install call takes every exe row");

static OffMainFrame s_om;                   // main thread
static LONGLONG     s_particlesT0 = 0;      // main: the particles row's entry, 0 = none this frame
static LONGLONG     s_fxForkT0    = 0;      // main: the fork row's entry
static bool         s_fpSubWind   = false;  // main: the sub-batch build in flight is a wind one
static bool         s_fpLockRO    = false;  // main: the lock in flight is read-only
static const char*  s_status      = "off";

// The particle job's executes since the fork row's entry: any Ogre worker adds, the main thread
// reads them once the blocking fork has returned.
static volatile LONG64 s_jobSum      = 0;
static volatile LONG64 s_jobMax      = 0;
static volatile LONG64 s_jobMinStart = LLONG_MAX;
static volatile LONG64 s_jobMaxEnd   = 0;

static void AtomicMin64(volatile LONG64* p, LONG64 v)
{
	LONG64 cur = *p;
	while (v < cur)
	{
		LONG64 prev = InterlockedCompareExchange64(p, v, cur);
		if (prev == cur)
			return;
		cur = prev;
	}
}

// Any thread: the job's slices run on Ogre's workers.
static unsigned __int64 hk_FxJob(void* job, int threadId, int threadCount)
{
	if (!g_cfg.offMainDetail)
		return oFxJob(job, threadId, threadCount);
	LONGLONG s = Now();
	unsigned __int64 r = oFxJob(job, threadId, threadCount);
	LONGLONG e = Now();
	InterlockedExchangeAdd64(&s_jobSum, e - s);
	AtomicMax64(&s_jobMax, e - s);
	AtomicMin64(&s_jobMinStart, s);
	AtomicMax64(&s_jobMaxEnd, e);
	return r;
}

static unsigned __int64 hk_FpBuild(void* batch)
{
	if (!IsMain() || !g_cur.open)
		return oFpBuild(batch);
	LONGLONG t0 = Now();
	unsigned __int64 r = oFpBuild(batch);
	s_om.fpBuild += Now() - t0;
	return r;
}

static void hk_FpTreeLoad(void* loader, void* page)
{
	if (!IsMain() || !g_cur.open)
	{
		oFpTreeLoad(loader, page);
		return;
	}
	LONGLONG t0 = Now();
	oFpTreeLoad(loader, page);
	s_om.fpTree += Now() - t0;
}

static void hk_FpGrass(void* loader, void* page)
{
	if (!IsMain() || !g_cur.open)
	{
		oFpGrass(loader, page);
		return;
	}
	LONGLONG t0 = Now();
	oFpGrass(loader, page);
	s_om.fpGrass += Now() - t0;
}

int InstallOffMainRows(HMODULE module, CallSiteProbe::Site* rows, const int* idx, int n)
{
	static CallSiteProbe::Site batch[MAX_ROW_SET];
	static int                 at[MAX_ROW_SET];
	int m = 0;
	for (int k = 0; k < n; ++k)
	{
		CallSiteProbe::Site& s = rows[idx[k]];
		s.id = -1;
		const char* skip = NameDisabled(s.name) ? "SKIP ini" : (m >= MAX_ROW_SET ? "SKIP row set full" : NULL);
		if (skip)
		{
			_snprintf_s(s.status, sizeof(s.status), _TRUNCATE, "%s", skip);
			AuditLine(Fmt("[Audit] site %s @0x%X %s", s.name, (unsigned)s.siteRva, skip));
			continue;
		}
		batch[m] = s;
		at[m]    = idx[k];
		++m;
	}
	int ok = m > 0 ? CallSiteProbe::Install(module, batch, m) : 0;
	for (int k = 0; k < m; ++k)
	{
		const CallSiteProbe::Site& s = batch[k];
		rows[at[k]] = s;
		std::string target = s.vslot ? Fmt("[vt+0x%X]", (unsigned)s.vslot)
		                   : s.indirect ? Fmt("[0x%X]", (unsigned)s.targetRva)
		                   : Fmt("0x%X", (unsigned)s.targetRva);
		AuditLine(Fmt("[Audit] site %s @0x%X -> %s %s", s.name, (unsigned)s.siteRva, target.c_str(), s.status));
	}
	return ok;
}

void MarkOffMainTags(const CallSiteProbe::Site* rows, int count)
{
	for (int i = 0; i < count; ++i)
	{
		int tag = rows[i].tag;
		if (tag < 0 || tag >= ST_COUNT)
			continue;
		bool all = true;
		for (int j = 0; j < count; ++j)
		{
			if (rows[j].tag == tag && rows[j].id < 0)
				all = false;
		}
		g_haveTag[tag] = all;
	}
}

void InstallOffMain(bool steam)
{
	if (!g_cfg.offMainDetail)
	{
		AuditLine("[Audit] offmain: off");
		return;
	}
	if (!steam)
	{
		AuditLine("[Audit] offmain: off (the call-site probes need Steam 1.0.65)");
		return;
	}
	// The pair InstallSites sets, set again for a run whose own table was empty.
	CallSiteProbe::SetCallbacks(&OnProbeEnter, &OnProbeExit);
	int idx[MAX_ROW_SET];
	for (int i = 0; i < NUM_EXE_SITES; ++i)
		idx[i] = i;
	int rows = InstallOffMainRows((HMODULE)g_base, s_exeSites, idx, NUM_EXE_SITES);
	MarkOffMainTags(s_exeSites, NUM_EXE_SITES);
	s_fxJobHooked   = AuditHookExe("fxJob", RVA_FX_JOB, PRO_FX_JOB, (void*)&hk_FxJob, (void**)&oFxJob);
	s_fpBuildHooked = AuditHookExe("fpBuild", RVA_FP_BUILD, PRO_FP_BUILD, (void*)&hk_FpBuild, (void**)&oFpBuild);
	s_fpTreeHooked  = AuditHookExe("fpTreeLoad", RVA_FP_TREE_LOAD, PRO_FP_TREE_LOAD,
	                               (void*)&hk_FpTreeLoad, (void**)&oFpTreeLoad);
	s_fpGrassHooked = AuditHookExe("fpGrass", RVA_FP_GRASS, PRO_FP_GRASS, (void*)&hk_FpGrass, (void**)&oFpGrass);
	int hooks = (s_fxJobHooked ? 1 : 0) + (s_fpBuildHooked ? 1 : 0) + (s_fpTreeHooked ? 1 : 0) + (s_fpGrassHooked ? 1 : 0);
	int ogre = InstallOgreProbes(GetModuleHandleA(OGRE_DLL));
	bool all = rows == NUM_EXE_SITES && hooks == NUM_HOOKS && ogre == NUM_OGRE_ROWS;
	s_status = all ? "on" : (rows + hooks + ogre > 0 ? "partial" : "off");
	AuditLine(Fmt("[Audit] offmain: %s (exe rows %d/%d, hooks %d/%d, Ogre part %s)", s_status,
	              rows, NUM_EXE_SITES, hooks, NUM_HOOKS, OffMainPartWord(ogre, NUM_OGRE_ROWS)));
}

const char* OffMainStatus()
{
	return s_status;
}

void OffMainProbeEnter(int tag, CallSiteProbe::U64 a, CallSiteProbe::U64 b, CallSiteProbe::U64, CallSiteProbe::U64)
{
	if (tag >= ST_SCENE_FIRST)
	{
		SceneProbeEnter(tag, a, b);
		return;
	}
	if (tag >= ST_XF_FIRST)
	{
		OgreXfEnter(tag, a, b);
		return;
	}
	if (tag >= ST_OGRE_WORKER_FIRST)
	{
		OgreProbeEnter(tag, a, b);
		return;
	}
	if (!IsMain() || !g_cur.open)
		return;
	switch (tag)
	{
	case ST_FX_FORK:
		// No slice runs before the fork, so the words start clean.
		s_fxForkT0 = Now();
		InterlockedExchange64(&s_jobSum, 0);
		InterlockedExchange64(&s_jobMax, 0);
		InterlockedExchange64(&s_jobMinStart, LLONG_MAX);
		InterlockedExchange64(&s_jobMaxEnd, 0);
		break;
	case ST_FP_SUB:
		s_fpSubWind = PlausiblePtr((uintptr_t)a) && *(const uintptr_t*)a == g_base + WIND_SUBBATCH_VT;
		break;
	case ST_FP_LOCK:   // (buffer, LockOptions, UploadOptions)
		s_fpLockRO = syncsplit::IsReadOnlyLock((int)b);
		break;
	default:
		break;
	}
}

// The fork row's exit: the fork blocked until every slice ran, so the job words are final.
static void FxForkExit(LONGLONG t0, LONGLONG t1)
{
	OffMainFrame& s = s_om;
	s.fxFork += t1 - t0;
	if (s_particlesT0 != 0 && t0 >= s_particlesT0)
		s.fxPre += t0 - s_particlesT0;
	s_particlesT0 = 0;
	LONG64 first = s_jobMinStart;
	s.fxJobSum += s_jobSum;
	if (s_jobMax > s.fxJobMax)
		s.fxJobMax = s_jobMax;
	if (first != LLONG_MAX && s_fxForkT0 != 0 && first >= s_fxForkT0)   // no slice ran: no wake, no tail
	{
		s.fxWake += first - s_fxForkT0;
		s.fxTail += t1 - s_jobMaxEnd;
	}
}

void OffMainProbeExit(int tag, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1)
{
	if (tag >= ST_SCENE_FIRST)
	{
		SceneProbeExit(tag, ret, t0, t1);
		return;
	}
	if (tag >= ST_XF_FIRST)
	{
		OgreXfExit(tag, t0, t1);
		return;
	}
	if (tag >= ST_OGRE_WORKER_FIRST)
	{
		OgreProbeExit(tag, t0, t1);
		return;
	}
	if (!IsMain() || !g_cur.open)
		return;
	OffMainFrame& s = s_om;
	LONGLONG d = t1 - t0;
	switch (tag)
	{
	case ST_FX_FORK:        FxForkExit(t0, t1); break;
	case ST_FX_DRAIN:       s.fxDrain += d; ++s.fxDrainN; break;
	case ST_FP_LOAD:        s.fpLoad += d; ++s.fpLoadN; break;
	case ST_FP_LOADCACHE:   s.fpLoadCache += d; ++s.fpLoadCacheN; break;
	case ST_FP_SUB:
		if (s_fpSubWind)
		{
			s.fpSubWind += d;
			++s.fpSubWindN;
		}
		else
		{
			s.fpSub += d;
			++s.fpSubN;
		}
		break;
	case ST_FP_POOLWAIT:    s.fpPoolWait += d; break;
	case ST_FP_LOCK:
		if (s_fpLockRO)
			s.fpLockRO += d;
		else
			s.fpLockDst += d;
		++s.fpLockN;
		break;
	case ST_MS_TRACE:       s.msTrace += d; break;
	case ST_MS_INDOORS:     s.msIndoors += d; break;
	case ST_MS_NAVVALID:    s.msNavValid += d; break;
	case ST_MS_INDOORSFAST: s.msIndoorsFast += d; break;
	case ST_MS_TERRAIN:     s.msTerrain += d; break;
	case ST_OA_FIRE:
	case ST_OA_WAIT:        OgreProbeExit(tag, t0, t1); break;
	default:                break;
	}
}

void OffMainParticlesEnter()
{
	if (g_cfg.offMainDetail && IsMain() && g_cur.open)
		s_particlesT0 = Now();
}

void OffMainIndoorsOther(LONGLONG ticks, bool physicsRunning)
{
	if (!g_cfg.offMainDetail || !IsMain() || !g_cur.open)
		return;
	s_om.indoorsOther += ticks;
	++s_om.indoorsOtherN;
	if (physicsRunning)
		++s_om.indoorsOtherPhys;
}

static float OmMs(LONGLONG ticks, bool have)
{
	return have ? TicksToMs(ticks) : Nan();
}

void OffMainFrameTotals(FrameRec& r)
{
	const OffMainFrame& s = s_om;
	bool fork = g_haveTag[ST_FX_FORK];
	bool job  = fork && s_fxJobHooked;
	bool sub  = g_haveTag[ST_FP_SUB];
	bool lock = g_haveTag[ST_FP_LOCK];
	r.m[M_OM_FXFORK]        = OmMs(s.fxFork, fork);
	r.m[M_OM_FXPRE]         = OmMs(s.fxPre, fork && g_haveTag[ST_PARTICLES]);
	r.m[M_OM_FXDRAIN]       = OmMs(s.fxDrain, g_haveTag[ST_FX_DRAIN]);
	r.m[M_OM_FXJOBMAX]      = OmMs(s.fxJobMax, job);
	r.m[M_OM_FXJOBSUM]      = OmMs(s.fxJobSum, job);
	r.m[M_OM_FXWAKE]        = OmMs(s.fxWake, job);
	r.m[M_OM_FXTAIL]        = OmMs(s.fxTail, job);
	r.m[M_OM_FPLOAD]        = OmMs(s.fpLoad, g_haveTag[ST_FP_LOAD]);
	r.m[M_OM_FPLOADCACHE]   = OmMs(s.fpLoadCache, g_haveTag[ST_FP_LOADCACHE]);
	r.m[M_OM_FPTREE]        = OmMs(s.fpTree, s_fpTreeHooked);
	r.m[M_OM_FPGRASS]       = OmMs(s.fpGrass, s_fpGrassHooked);
	r.m[M_OM_FPBUILD]       = OmMs(s.fpBuild, s_fpBuildHooked);
	r.m[M_OM_FPSUB]         = OmMs(s.fpSub, sub);
	r.m[M_OM_FPSUBWIND]     = OmMs(s.fpSubWind, sub);
	r.m[M_OM_FPPOOLWAIT]    = OmMs(s.fpPoolWait, g_haveTag[ST_FP_POOLWAIT]);
	r.m[M_OM_FPLOCKRO]      = OmMs(s.fpLockRO, lock);
	r.m[M_OM_FPLOCKDST]     = OmMs(s.fpLockDst, lock);
	r.m[M_OM_MSTRACE]       = OmMs(s.msTrace, g_haveTag[ST_MS_TRACE]);
	r.m[M_OM_MSINDOORS]     = OmMs(s.msIndoors, g_haveTag[ST_MS_INDOORS]);
	r.m[M_OM_MSNAVVALID]    = OmMs(s.msNavValid, g_haveTag[ST_MS_NAVVALID]);
	r.m[M_OM_MSINDOORSFAST] = OmMs(s.msIndoorsFast, g_haveTag[ST_MS_INDOORSFAST]);
	r.m[M_OM_MSTERRAIN]     = OmMs(s.msTerrain, g_haveTag[ST_MS_TERRAIN]);
	r.m[M_OM_INDOORSOTHER]  = OmMs(s.indoorsOther, oIsIndoors != NULL);   // the isIndoors hook rides PhysXDetail
	r.c[C_FXDRAINN]         = s.fxDrainN;
	r.c[C_FPLOADN]          = s.fpLoadN;
	r.c[C_FPLOADCACHEN]     = s.fpLoadCacheN;
	r.c[C_FPSUBN]           = s.fpSubN;
	r.c[C_FPSUBWINDN]       = s.fpSubWindN;
	r.c[C_FPLOCKN]          = s.fpLockN;
	r.c[C_INDOORSOTHERN]    = s.indoorsOtherN;
	r.c[C_INDOORSOTHERPHYS] = s.indoorsOtherPhys;
	memset(&s_om, 0, sizeof(s_om));
	s_particlesT0 = 0;
	OgreFrameTotals(r);
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
