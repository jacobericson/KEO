// audit_scene.cpp - The scene and render probes' OgreMain half (SceneDetail=1), all on the main
// thread. At each fire of the main scene manager's barrier, the inputs of the fork it starts: the
// render queues the cull walks, the version-2 skeleton managers, the objects whose bounds are
// updated, the instance batches, and the visible objects by class. Two call-site rows
// (prepareRenderQueue and the instance-buffer upload) and two entry hooks (the render-queue clear,
// which walks the pass-group map, and Entity::_updateRenderQueue). The D3D11 half is audit_d3d.cpp.
// The rows and hooks go in only for the OgreMain build their offsets were read from.
// No hook or callback takes a lock, allocates, or logs; the [AUDIT-RQ] line is written from the
// once-a-second pass.

#include "audit_scene.h"
#include "audit_offmain.h"
#include "audit_scene_rules.h"

namespace audit_scene_detail {

// Main-thread totals of the frame in flight: SceneFrameTotals writes them out and clears them.
struct SceneFrame
{
	LONGLONG           rqPrep, instUpd, instUpdEmpty;
	unsigned long long rquEnt;
	int cullRq, cullRqEmpty, v2SkelMgrs, bndEntObjs, bndLightObjs, instDynB, instDirtyB, instCullForks;
	int instCullEmpty, visEnt, visBatch, visOther, rqClears, rqMapWalk, rqMapMax, rquEntN, instUpdN, instEmptyN;
};

// The [AUDIT-RQ] window: the clears, and the full map walks of the sampled ones.
struct RqWindow
{
	int                clears, sampled, sizeBad;
	unsigned long long walked, nonEmpty;
};

} // audit_scene_detail
using namespace audit_scene_detail;

namespace kenshiframeaudit_detail {

using CallSiteProbe::SHAPE_INT;

// SceneManager: the fork inputs, read at the fire.
static const size_t SM_CULL_REQUEST = 0x4A30;   // request 0: the first and last render queue (bytes), the managers
static const size_t CULL_FIRST_RQ   = 0x00;
static const size_t CULL_LAST_RQ    = 0x01;
static const size_t CULL_MANAGERS   = 0x08;     // a vector of ObjectMemoryManager*
static const size_t SM_SKEL_BEGIN   = 0x3F8;    // request 1: the version-2 skeleton managers
static const size_t SM_SKEL_END     = 0x400;
static const size_t SM_BOUNDS_LIST  = 0x4AE8;   // request 3: the vector of managers it bounds
static const size_t SM_ENTITY_LIST  = 0x3B8;    // ... the entities' vector
static const size_t SM_LIGHT_LIST   = 0x3D8;    // ... the lights' vector
static const size_t SM_INST_BEGIN   = 0x578;    // request 7: the instance managers
static const size_t SM_INST_END     = 0x580;
static const size_t SM_VISIBLE      = 0x4B48;   // request 8: the visible lists, and their count
static const size_t SM_VISIBLE_N    = 0x4B50;
static const size_t SM_REQUEST      = 0x4B18;   // the request the workers switch on
static const size_t SM_BARRIER      = 0x4B20;   // the worker barrier
static const size_t BARRIER_INDEX   = 0x08;
// ObjectMemoryManager: its per-render-queue blocks, 160 bytes each with the object count at +64.
static const size_t MGR_BLOCKS      = 0x08;
static const size_t RQ_BLOCK_STRIDE = 160;
static const size_t RQ_BLOCK_COUNT  = 64;
// InstanceManager: the dynamic batches and the dirty static batches, 8 bytes each.
static const size_t IM_DYN_BEGIN    = 72;
static const size_t IM_DYN_END      = 80;
static const size_t IM_DIRTY_BEGIN  = 104;
static const size_t IM_DIRTY_END    = 112;
// A visible list: { data, size, capacity }, each entry a MovableObject*; vtable slots +0x40
// (_updateRenderQueue) and +0x48 (instanceBatchCullFrustumThreaded).
static const size_t VIS_STRIDE      = 24;
static const size_t VIS_SIZE        = 8;
static const size_t VT_QUEUE_SLOT   = 0x40 / 8;
static const size_t VT_CULL_SLOT    = 0x48 / 8;
// QueuedRenderableCollection: the pass-group map's head and size.
static const size_t RQC_MAP_HEAD    = 0x10;
static const size_t RQC_MAP_SIZE    = 0x18;

// OgreMain_x64.dll (the build OgreMainMatches checks).
static const size_t OGRE_ENT_UPDATE_RQ = 0xC1080;    // Entity::_updateRenderQueue
static const size_t OGRE_HW_CULL       = 0x12FBA0;   // InstanceBatchHW::instanceBatchCullFrustumThreaded
static const size_t OGRE_VTF_CULL      = 0x130E90;   // InstanceBatchHW_VTF::instanceBatchCullFrustumThreaded
static const size_t OGRE_NUM_RQ        = 0x3D6DD0;   // ObjectMemoryManager::getNumRenderQueues
static const char* const SYM_NUM_RQ    ="?getNumRenderQueues@ObjectMemoryManager@Ogre@@QEBA_KXZ";
static const size_t OGRE_RQ_CLEAR      = 0x24A460;   // QueuedRenderableCollection::clear
static const unsigned char PRO_RQ_CLEAR[16]  = { 0x41,0x55,0x48,0x83,0xEC,0x30,0x48,0x89,0x5C,0x24,0x40,0x48,0x89,0x74,0x24,0x50 };
static const unsigned char PRO_ENT_UPDRQ[16] = { 0x48,0x89,0x6C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x80,0xB9,0x08,0x03,0x00,0x00 };

// Bounds on every walk: a larger count is a bad read or is cut off.
static const size_t MAX_MGRS     = 64;
static const size_t MAX_RQ       = 256;
static const size_t MAX_LISTS    = 64;
static const size_t MAX_OBJS     = 65536;
static const size_t MAP_WALK_MAX = 1000000;
static const int    MAP_SAMPLE   = 256;   // one clear in this many walks its map in full

typedef size_t (*NumRq_t)(const void*);
typedef void   (*RqClear_t)(void*);
typedef void   (*EntUpdRq_t)(void*, void*, void*, const void*);

// Call sites in OgreMain_x64.dll (RVAs from its image base).
static CallSiteProbe::Site s_sceneSites[] =
{
	{ "rqPrep",  0x2BD308, 0,        SHAPE_INT, ST_RQ_PREP,    0x98, 0, 1 },  // _renderPhase02 -> prepareRenderQueue
	{ "instUpd", 0x1307C2, 0x130490, SHAPE_INT, ST_RQ_INSTUPD },              // InstanceBatchHW::updateVertexBuffer
};
static const int NUM_SCENE_SITES = sizeof(s_sceneSites) / sizeof(s_sceneSites[0]);
static const int NUM_SCENE_HOOKS = 2;

static NumRq_t     s_numRq     = NULL;
static RqClear_t   oRqClear    = NULL;
static EntUpdRq_t  oEntUpdRq   = NULL;
static bool        s_entHooked = false;
static uintptr_t   s_ogreBase  = 0;      // set once the OgreMain part passed its checks
static const char* s_status    = "off";

// Main thread.
static SceneFrame s_sc;
static RqWindow   s_win;
static LONGLONG   s_winStart = 0;
static int        s_passes   = 0;
static unsigned   s_clearSeq = 0;
static int        s_entDepth = 0;

static int CapInt(size_t n)
{
	return (int)(n < MAX_OBJS * MAX_LISTS ? n : MAX_OBJS * MAX_LISTS);
}

// [begin, end) of a vector of 8-byte entries whose begin and end pointers sit at beginAt and
// endAt: the entry count up to `cap`, 0 for an empty or implausible one.
static size_t Span(uintptr_t beginAt, uintptr_t endAt, size_t cap, uintptr_t* begin)
{
	uintptr_t b = *(const uintptr_t*)beginAt;
	uintptr_t e = *(const uintptr_t*)endAt;
	if (!PlausiblePtr(b) || !PlausiblePtr(e) || e < b)
		return 0;
	*begin = b;
	size_t n = (e - b) / 8;
	return n < cap ? n : cap;
}

// One ObjectMemoryManager's render queues in [first, min(its count, last)).
static void CountManager(uintptr_t mgr, size_t first, size_t last, scenerules::QueueCount* q)
{
	if (!PlausiblePtr(mgr))
		return;
	size_t n = s_numRq((const void*)mgr);
	if (n > MAX_RQ)
		n = MAX_RQ;
	uintptr_t blocks = *(const uintptr_t*)(mgr + MGR_BLOCKS);
	if (n == 0 || !PlausiblePtr(blocks))
		return;
	scenerules::CountQueues((const unsigned char*)blocks, RQ_BLOCK_STRIDE, RQ_BLOCK_COUNT, first, last, n, q);
}

// Every manager of the vector at `vec` (begin and end at +0 and +8).
static scenerules::QueueCount CountManagers(uintptr_t vec, size_t first, size_t last)
{
	scenerules::QueueCount q = { 0, 0, 0 };
	uintptr_t b = 0;
	size_t n = PlausiblePtr(vec) ? Span(vec, vec + 8, MAX_MGRS, &b) : 0;
	for (size_t i = 0; i < n; ++i)
		CountManager(*(const uintptr_t*)(b + i * 8), first, last, &q);
	return q;
}

// Request 1: the version-2 skeleton managers whose list (a sentinel linked to itself when empty)
// holds a skeleton.
static void SkeletonInputs(uintptr_t sm)
{
	uintptr_t b = 0;
	size_t n = Span(sm + SM_SKEL_BEGIN, sm + SM_SKEL_END, MAX_MGRS, &b);
	for (size_t i = 0; i < n; ++i)
	{
		uintptr_t entry = *(const uintptr_t*)(b + i * 8);
		if (!PlausiblePtr(entry))
			continue;
		uintptr_t sentinel = *(const uintptr_t*)entry;
		if (PlausiblePtr(sentinel) && *(const uintptr_t*)sentinel != sentinel)
			++s_sc.v2SkelMgrs;
	}
}

// Request 7: each instance manager's dynamic and dirty static batches.
static void InstanceInputs(uintptr_t sm)
{
	uintptr_t b = 0;
	size_t n = Span(sm + SM_INST_BEGIN, sm + SM_INST_END, MAX_MGRS, &b);
	for (size_t i = 0; i < n; ++i)
	{
		uintptr_t mgr = *(const uintptr_t*)(b + i * 8);
		if (!PlausiblePtr(mgr))
			continue;
		uintptr_t first = 0;
		s_sc.instDynB   += (int)Span(mgr + IM_DYN_BEGIN, mgr + IM_DYN_END, MAX_OBJS, &first);
		s_sc.instDirtyB += (int)Span(mgr + IM_DIRTY_BEGIN, mgr + IM_DIRTY_END, MAX_OBJS, &first);
	}
}

// Request 8: every visible object by class; a pass with no instance batch is an empty cull fork.
static void VisibleInputs(uintptr_t sm)
{
	++s_sc.instCullForks;
	bool batch = false;
	uintptr_t lists = *(const uintptr_t*)(sm + SM_VISIBLE);
	size_t nl = *(const size_t*)(sm + SM_VISIBLE_N);
	if (!PlausiblePtr(lists) || nl > MAX_LISTS)
		nl = 0;
	size_t seen = 0;
	for (size_t l = 0; l < nl && seen < MAX_OBJS; ++l)
	{
		uintptr_t data = *(const uintptr_t*)(lists + l * VIS_STRIDE);
		size_t size = *(const size_t*)(lists + l * VIS_STRIDE + VIS_SIZE);
		if (!PlausiblePtr(data))
			continue;
		for (size_t j = 0; j < size && seen < MAX_OBJS; ++j, ++seen)
		{
			uintptr_t obj = *(const uintptr_t*)(data + j * 8);
			uintptr_t vt = PlausiblePtr(obj) ? *(const uintptr_t*)obj : 0;
			if (!PlausiblePtr(vt))
				continue;
			const size_t* slots = (const size_t*)vt;
			int cls = scenerules::VisClass(slots[VT_QUEUE_SLOT], slots[VT_CULL_SLOT], s_ogreBase + OGRE_ENT_UPDATE_RQ,
			                               s_ogreBase + OGRE_HW_CULL, s_ogreBase + OGRE_VTF_CULL);
			if (cls == scenerules::VC_BATCH)
			{
				++s_sc.visBatch;
				batch = true;
			}
			else if (cls == scenerules::VC_ENTITY)
				++s_sc.visEnt;
			else
				++s_sc.visOther;
		}
	}
	if (!batch)
		++s_sc.instCullEmpty;
}

void SceneSyncInputs(void* barrier)
{
	uintptr_t sm = (uintptr_t)g_sceneMgr;
	if (s_ogreBase == 0 || !PlausiblePtr(sm) || (uintptr_t)barrier != *(const uintptr_t*)(sm + SM_BARRIER) ||
	    syncsplit::KindOf(*(const int*)((uintptr_t)barrier + BARRIER_INDEX)) != syncsplit::SK_FIRE)
		return;
	switch (*(const int*)(sm + SM_REQUEST))
	{
	case 0:
	{
		uintptr_t req = sm + SM_CULL_REQUEST;
		scenerules::QueueCount q = CountManagers(*(const uintptr_t*)(req + CULL_MANAGERS),
		                                         *(const unsigned char*)(req + CULL_FIRST_RQ),
		                                         *(const unsigned char*)(req + CULL_LAST_RQ));
		s_sc.cullRq      += q.queues;
		s_sc.cullRqEmpty += q.empty;
		break;
	}
	case 1:
		SkeletonInputs(sm);
		break;
	case 3:
	{
		uintptr_t list = *(const uintptr_t*)(sm + SM_BOUNDS_LIST);
		int objs = CapInt(CountManagers(list, 0, MAX_RQ).objects);
		if (list == sm + SM_LIGHT_LIST)
			s_sc.bndLightObjs += objs;
		else if (list == sm + SM_ENTITY_LIST)
			s_sc.bndEntObjs += objs;
		break;
	}
	case 7:
		InstanceInputs(sm);
		break;
	case 8:
		VisibleInputs(sm);
		break;
	default:
		break;
	}
}

// ---- The render queue ----

void SceneProbeEnter(int, CallSiteProbe::U64, CallSiteProbe::U64)
{
	// Nothing at entry: each row's exit has what it needs.
}

void SceneProbeExit(int tag, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1)
{
	if (!IsMain())
		return;
	if (tag >= ST_D3D_BLEND)
	{
		D3dProbeExit(tag, t0, t1);   // ClearState forgets the shadows with the frame closed too
		return;
	}
	if (!g_cur.open || g_depth <= 0)
		return;
	LONGLONG d = t1 - t0;
	if (tag == ST_RQ_PREP)
		s_sc.rqPrep += d;
	else if (tag == ST_RQ_INSTUPD)
	{
		s_sc.instUpd += d;
		++s_sc.instUpdN;
		if (ret == 0)   // no instance survived the cull
		{
			s_sc.instUpdEmpty += d;
			++s_sc.instEmptyN;
		}
	}
}

// The map is walked before the original clears it, while it still holds this pass's lists.
static void hk_RqClear(void* coll)
{
	uintptr_t c = (uintptr_t)coll;
	if (g_cfg.sceneDetail && IsMain() && g_cur.open && g_depth > 0 && PlausiblePtr(c))
	{
		size_t size = *(const size_t*)(c + RQC_MAP_SIZE);
		if (size > MAP_WALK_MAX)
			++s_win.sizeBad;
		else
		{
			++s_sc.rqClears;
			++s_win.clears;
			s_sc.rqMapWalk += (int)size;
			if ((int)size > s_sc.rqMapMax)
				s_sc.rqMapMax = (int)size;
			uintptr_t head = *(const uintptr_t*)(c + RQC_MAP_HEAD);
			if (++s_clearSeq % MAP_SAMPLE == 0 && PlausiblePtr(head))
			{
				size_t nonEmpty = 0;
				size_t walked = scenerules::MapWalk((const unsigned char*)head, MAP_WALK_MAX, &nonEmpty);
				++s_win.sampled;
				s_win.walked   += walked;
				s_win.nonEmpty += nonEmpty;
				if (walked != size)
					++s_win.sizeBad;
			}
		}
	}
	oRqClear(coll);
}

// The outermost entity call inside a scene call; one inside it (an attached entity) is its own time.
static void hk_EntUpdRq(void* ent, void* queue, void* cam, const void* lodCam)
{
	if (!IsMain() || !g_cur.open || g_depth <= 0 || s_entDepth != 0)
	{
		oEntUpdRq(ent, queue, cam, lodCam);
		return;
	}
	++s_entDepth;
	unsigned long long t0 = __rdtsc();
	oEntUpdRq(ent, queue, cam, lodCam);
	unsigned long long dt = __rdtsc() - t0;
	--s_entDepth;
	s_sc.rquEnt += dt;
	++s_sc.rquEntN;
}

// ---- Install, frame record and the [AUDIT-RQ] line ----

// The OgreMain part's refusal, or NULL with its rows and hooks counted.
static const char* InstallSceneOgre(HMODULE ogre, int* rows, int* hooks)
{
	if (!ogre)
		return "no module";
	if (!OgreMainMatches(ogre))
		return "build mismatch";
	uintptr_t base = (uintptr_t)ogre;
	s_numRq = (NumRq_t)GetProcAddress(ogre, SYM_NUM_RQ);
	if ((uintptr_t)s_numRq != base + OGRE_NUM_RQ)
	{
		s_numRq = NULL;
		return "getNumRenderQueues not at its offset";
	}
	s_ogreBase = base;
	int idx[NUM_SCENE_SITES];
	for (int i = 0; i < NUM_SCENE_SITES; ++i)
		idx[i] = i;
	*rows = InstallOffMainRows(ogre, s_sceneSites, idx, NUM_SCENE_SITES);
	MarkOffMainTags(s_sceneSites, NUM_SCENE_SITES);
	bool clear = HookAt("rqClear", ogre, (void*)(base + OGRE_RQ_CLEAR), PRO_RQ_CLEAR,
	                    (void*)&hk_RqClear, (void**)&oRqClear);
	s_entHooked = HookAt("entUpdRq", ogre, (void*)(base + OGRE_ENT_UPDATE_RQ), PRO_ENT_UPDRQ,
	                     (void*)&hk_EntUpdRq, (void**)&oEntUpdRq);
	*hooks = (clear ? 1 : 0) + (s_entHooked ? 1 : 0);
	return NULL;
}

void InstallScene()
{
	s_winStart = Now();
	if (!g_cfg.sceneDetail)
	{
		AuditLine("[Audit] scene: off");
		return;
	}
	const char* why = !IsMain() ? "not the main thread"
	                : (!g_renderOn || !g_syncHooked || !g_d3dHooked) ? "needs RenderDetail=1 RenderDeep=1"
	                : NULL;
	if (why)
	{
		AuditLine(Fmt("[Audit] scene: off (%s)", why));
		return;
	}
	// The pair InstallSites sets, set again for a run whose own tables were empty.
	CallSiteProbe::SetCallbacks(&OnProbeEnter, &OnProbeExit);
	int rows = 0, hooks = 0;
	const char* ogreWhy = InstallSceneOgre(GetModuleHandleA(OGRE_DLL), &rows, &hooks);
	if (ogreWhy)
		AuditLine(Fmt("[Audit] scene: Ogre part off (%s)", ogreWhy));
	else
		AuditLine(Fmt("[Audit] scene: Ogre part %s (%d/%d rows, %d/%d hooks)",
		              OffMainPartWord(rows + hooks, NUM_SCENE_SITES + NUM_SCENE_HOOKS), rows, NUM_SCENE_SITES,
		              hooks, NUM_SCENE_HOOKS));
	HMODULE d3d = GetModuleHandleA(D3D11_DLL);
	int d3dRows = 0;
	const char* d3dWhy = InstallD3dProbes(d3d, &d3dRows);
	if (d3dRows == 0)
		AuditLine(Fmt("[Audit] scene: D3D11 part off (%s)", d3dWhy ? d3dWhy : "no row installed"));
	else
		AuditLine(Fmt("[Audit] scene: D3D11 part %s (%d/%d rows, page=%p)", OffMainPartWord(d3dRows, NUM_D3D_SITES),
		              d3dRows, NUM_D3D_SITES, CallSiteProbe::StubPageFor(d3d)));
	int have = rows + hooks + d3dRows;
	s_status = have == NUM_SCENE_SITES + NUM_SCENE_HOOKS + NUM_D3D_SITES ? "on" : (have > 0 ? "partial" : "off");
}

const char* SceneStatus()
{
	return s_status;
}

static float ScMs(LONGLONG ticks, bool have)
{
	return have ? TicksToMs(ticks) : Nan();
}

void SceneFrameTotals(FrameRec& r)
{
	const SceneFrame& s = s_sc;
	double perMs = g_tscPerMs;
	bool inst = g_haveTag[ST_RQ_INSTUPD];
	r.m[M_SC_RQPREP]       = ScMs(s.rqPrep, g_haveTag[ST_RQ_PREP]);
	r.m[M_SC_RQUENT]       = s_entHooked && perMs > 0 ? (float)((double)s.rquEnt / perMs) : Nan();
	r.m[M_SC_INSTUPD]      = ScMs(s.instUpd, inst);
	r.m[M_SC_INSTUPDEMPTY] = ScMs(s.instUpdEmpty, inst);
	r.c[C_CULLRQ]        = s.cullRq;
	r.c[C_CULLRQEMPTY]   = s.cullRqEmpty;
	r.c[C_V2SKELMGRS]    = s.v2SkelMgrs;
	r.c[C_BNDENTOBJS]    = s.bndEntObjs;
	r.c[C_BNDLIGHTOBJS]  = s.bndLightObjs;
	r.c[C_INSTDYNB]      = s.instDynB;
	r.c[C_INSTDIRTYB]    = s.instDirtyB;
	r.c[C_INSTCULLFORKS] = s.instCullForks;
	r.c[C_INSTCULLEMPTY] = s.instCullEmpty;
	r.c[C_VISENT]        = s.visEnt;
	r.c[C_VISBATCH]      = s.visBatch;
	r.c[C_VISOTHER]      = s.visOther;
	r.c[C_RQCLEARS]      = s.rqClears;
	r.c[C_RQMAPWALK]     = s.rqMapWalk;
	r.c[C_RQMAPMAX]      = s.rqMapMax;
	r.c[C_RQUENTN]       = s.rquEntN;
	r.c[C_INSTUPDN]      = s.instUpdN;
	r.c[C_INSTEMPTYN]    = s.instEmptyN;
	D3dFrameTotals(r);
	memset(&s_sc, 0, sizeof(s_sc));
}

// Every fifth pass: the window's clears and the full walks that checked the map's size word.
void SceneOncePerSecond()
{
	if (s_ogreBase == 0 || ++s_passes < 5)
		return;
	s_passes = 0;
	LONGLONG now = Now();
	AuditLine(Fmt("[AUDIT-RQ] win=%.1fs clears=%d sampled=%d walked=%llu nonEmpty=%llu sizeBad=%d",
	              (double)(now - s_winStart) / (double)g_qpcFreq, s_win.clears, s_win.sampled, s_win.walked,
	              s_win.nonEmpty, s_win.sizeBad));
	memset(&s_win, 0, sizeof(s_win));
	s_winStart = now;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
