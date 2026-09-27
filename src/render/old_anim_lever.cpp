#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/module_hooks.h"
#include "game/klib_member_contract.h"
#include "base/core.h"
#include <iomanip>
#include <sstream>

// SceneManager::updateAllOldAnimations runs once per culled pass on the
// render thread, after the cull's own fork has joined. It queues every
// visible skeletal Entity, forks the worker pool twice to run
// Entity::updateAnimation on them, then, back on this thread, pushes every
// active tag point into its attached object's node and fires the frame
// listeners' framePostAnimationUpdates. The forks are the cost; the tag
// points and the listeners are not optional.
//
// The lever scans the same visible lists first with updateAnimation's own
// early-out. When no queued entity would update, the forks would change
// nothing, so it runs only the tail (tag points, listeners) and returns.

#ifdef ZONEOPT_DEBUG
static const bool DEV_BUILD = true;
#else
static const bool DEV_BUILD = false;
#endif

// Kenshi's SceneManager and OldSkeletonInstance, as updateAllOldAnimations
// reads them (both differ from the Ogre 2.0 headers).
static const size_t SM_WORKER_THREADS  = 0x4A28;  // size_t
static const size_t SM_VISIBLE_OBJECTS = 0x4B48;  // per-thread arrays of {data, size, capacity}
static const size_t VISIBLE_STRIDE     = 24;
static const size_t VISIBLE_SIZE       = 8;
static const size_t SKEL_TAG_POINTS    = 0x2A8;   // sentinel of a circular node list, next at +0
static const size_t TAG_NODE_VALUE     = 0x18;    // TagPoint*

// Kenshi's Ogre::Entity: the headers, built with NDEBUG as this DLL is,
// place these 8 bytes lower.
static const size_t ENT_ANIMATION_STATE = 0x1A8;  // AnimationStateSet*
static const size_t ENT_ANIM_UPDATED    = 0x284;  // unsigned long: frame the animation was last applied
static const size_t ENT_SKELETON        = 0x300;  // OldSkeletonInstance*
static const size_t ENT_INITIALISED     = 0x308;  // bool
static const size_t ENT_LAST_XFORM      = 0x30C;  // Matrix4: parent transform last applied

static const size_t VT_GET_MOVABLE_TYPE   = 0x20;   // MovableObject: const std::string& ()
static const size_t VT_MANUAL_BONES_DIRTY = 0x248;  // OldSkeletonInstance: bool ()

// MSVC std::string: inline buffer or heap pointer at +0.
static const size_t STR_SIZE   = 0x10;
static const size_t STR_CAP    = 0x18;
static const size_t STR_INLINE = 16;

static const unsigned TYPE_HASH_SEED    = 0x3A8EFA67;
static const int      POST_ANIM_MIN_OBJ = 15;   // listeners fire when thread 0 saw more
static const size_t   MATRIX4_SIZE      = 64;

typedef void        (*UpdateAllOldAnims_t)(void* scene);
typedef const void* (*GetMovableType_t)(const void* obj);
typedef bool        (*ManualBonesDirty_t)(const void* skel);
typedef void        (*Murmur3_t)(const void* key, int len, unsigned seed, void* out);
typedef bool        (*MatrixNotEqual_t)(const void* a, const void* b);
typedef void        (*TagUpdateNode_t)(const void* tag);
typedef void        (*FirePostAnim_t)(void* root);

static UpdateAllOldAnims_t s_orig          = NULL;
static const void*         s_entityType    = NULL;   // EntityFactory::FACTORY_TYPE_NAME
static unsigned            s_entityHash    = 0;      // its type-name hash
static Murmur3_t           s_murmur        = NULL;
static MatrixNotEqual_t    s_matrixNe      = NULL;
static TagUpdateNode_t     s_tagUpdateNode = NULL;
static FirePostAnim_t      s_firePostAnim  = NULL;
static void* const*        s_rootSingleton = NULL;
static bool                s_tried         = false;
static bool                s_installed     = false;

static volatile LONG     s_calls     = 0;
static volatile LONG     s_clean     = 0;   // scans that found nothing to update
static volatile LONGLONG s_scanTicks = 0;

// The skeletal entities of the last clean scan, in queue order. Render
// thread only; a scan that finds more falls back to the original.
static const int   MAX_SKELETAL = 4096;
static const char* s_skeletal[MAX_SKELETAL];
static int         s_skeletalCount = 0;

static const ModuleSite s_site =
{
	"SceneManager::updateAllOldAnimations", "OgreMain_x64.dll",
	"?updateAllOldAnimations@SceneManager@Ogre@@IEAAXXZ", 0x2CA920,
	{ 0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41 }
};

template<typename T> static T At(const void* base, size_t off)
{
	return *(const T*)((const char*)base + off);
}

static unsigned TypeHash(const void* str)
{
	const void* data = At<size_t>(str, STR_CAP) >= STR_INLINE ? At<const void*>(str, 0) : str;
	unsigned h = 0;
	s_murmur(data, (int)At<size_t>(str, STR_SIZE), TYPE_HASH_SEED, &h);
	return h;
}

// The original's Entity test compares type-name hashes; the name's own
// address is a shortcut to the same answer.
static bool IsEntity(const void* obj)
{
	GetMovableType_t getType = At<GetMovableType_t>(At<const void*>(obj, 0), VT_GET_MOVABLE_TYPE);
	const void* type = getType(obj);
	return type == s_entityType || TypeHash(type) == s_entityHash;
}

// Entity::updateAnimation's early-out, read for read: true when it would do
// any work. A missing pointer it dereferences unchecked counts as work, so
// the original runs.
static bool AnimationWouldUpdate(const char* e)
{
	if (!At<unsigned char>(e, ENT_INITIALISED))
		return false;
	const void* skel = At<const void*>(e, ENT_SKELETON);
	if (!skel)
		return false;
	const char* states = At<const char*>(e, ENT_ANIMATION_STATE);
	if (!states)
		return true;
	if (At<unsigned>(e, ENT_ANIM_UPDATED) !=
	    At<unsigned>(states, KLIB_OFF_AnimationStateSet_dirtyFrameNumber))
		return true;
	ManualBonesDirty_t manualDirty = At<ManualBonesDirty_t>(At<const void*>(skel, 0), VT_MANUAL_BONES_DIRTY);
	if (manualDirty(skel))
		return true;
	const char* node = At<const char*>(e, KLIB_OFF_MovableObject_parentNode);
	if (!node)
		return true;
	const char* xf = node + KLIB_OFF_Node_transform;
	const char* derived = At<const char*>(xf, KLIB_OFF_Transform_derivedTransform);
	if (!derived)
		return true;
	size_t index = At<unsigned char>(xf, KLIB_OFF_Transform_index);
	return s_matrixNe(e + ENT_LAST_XFORM, derived + index * MATRIX4_SIZE);
}

enum ScanResult { SCAN_CLEAN, SCAN_DIRTY, SCAN_FULL };

// Walks the visible lists as the original queues them: Entity-typed objects
// with a skeleton instance.
static ScanResult ScanVisible(const void* scene)
{
	s_skeletalCount = 0;
	size_t threads = At<size_t>(scene, SM_WORKER_THREADS);
	const char* arrays = At<const char*>(scene, SM_VISIBLE_OBJECTS);
	if (threads && !arrays)
		return SCAN_DIRTY;
	for (size_t t = 0; t < threads; ++t)
	{
		const char* arr = arrays + t * VISIBLE_STRIDE;
		const char* const* objs = At<const char* const*>(arr, 0);
		size_t count = At<size_t>(arr, VISIBLE_SIZE);
		for (size_t i = 0; i < count; ++i)
		{
			const char* obj = objs[i];
			if (!obj)
				return SCAN_DIRTY;
			if (!IsEntity(obj) || !At<const void*>(obj, ENT_SKELETON))
				continue;
			if (AnimationWouldUpdate(obj))
				return SCAN_DIRTY;
			if (s_skeletalCount == MAX_SKELETAL)
				return SCAN_FULL;
			s_skeletal[s_skeletalCount++] = obj;
		}
	}
	return SCAN_CLEAN;
}

// The original's work after its forks, for the entities of a clean scan.
static void RunTail(const void* scene)
{
	for (int i = 0; i < s_skeletalCount; ++i)
	{
		const char* skel = At<const char*>(s_skeletal[i], ENT_SKELETON);
		for (const char* n = At<const char*>(At<const char*>(skel, SKEL_TAG_POINTS), 0);
		     n != At<const char*>(skel, SKEL_TAG_POINTS); n = At<const char*>(n, 0))
			s_tagUpdateNode(At<const void*>(n, TAG_NODE_VALUE));
	}
	const char* arrays = At<const char*>(scene, SM_VISIBLE_OBJECTS);
	if (arrays && At<int>(arrays, VISIBLE_SIZE) > POST_ANIM_MIN_OBJ && *s_rootSingleton)
		s_firePostAnim(*s_rootSingleton);
}

// Render thread, once per culled pass: no allocation, lock or logging.
static void hook_UpdateAllOldAnimations(void* scene)
{
	bool diag = DEV_BUILD && g_renderCfg.oldAnimDiag;
	if (!diag && !g_renderCfg.oldAnimSkip)
	{
		s_orig(scene);
		return;
	}
	InterlockedIncrement(&s_calls);
	if (diag)
	{
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		ScanResult r = ScanVisible(scene);
		QueryPerformanceCounter(&t1);
		InterlockedExchangeAdd64(&s_scanTicks, t1.QuadPart - t0.QuadPart);
		if (r == SCAN_CLEAN)
			InterlockedIncrement(&s_clean);
		s_orig(scene);
		return;
	}
	if (ScanVisible(scene) != SCAN_CLEAN)
	{
		s_orig(scene);
		return;
	}
	InterlockedIncrement(&s_clean);
	RunTail(scene);
}

// 0 off, 1 the lever, 2 the diagnostic (which wins when both are on).
static int CurrentMode()
{
	if (DEV_BUILD && g_renderCfg.oldAnimDiag)
		return 2;
	return g_renderCfg.oldAnimSkip ? 1 : 0;
}

static void DrainCounts(LONG* calls, LONG* clean, LONGLONG* ticks)
{
	*calls = InterlockedExchange(&s_calls, 0);
	*clean = InterlockedExchange(&s_clean, 0);
	*ticks = InterlockedExchange64(&s_scanTicks, 0);
}

// Main thread, outside the render passes: a switch between off, the lever
// and the diagnostic zeroes the counters, so no window mixes two modes.
void OldAnim_MainThreadTick()
{
	static int modeWas = -1;   // -1 before the first tick
	if (!s_installed)
		return;
	int mode = CurrentMode();
	if (mode == modeWas)
		return;
	modeWas = mode;
	LONG calls, clean;
	LONGLONG ticks;
	DrainCounts(&calls, &clean, &ticks);
}

std::string OldAnimStatsToken(LONG loops)
{
	LONG calls, clean;
	LONGLONG ticks;
	DrainCounts(&calls, &clean, &ticks);
	int mode = CurrentMode();
	if (!s_installed || mode == 0)
		return std::string();
	double perLoop = loops > 0 ? 1.0 / (double)loops : 0.0;
	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(1) << " oldAnim=" << (double)calls * perLoop << "/"
	   << (double)clean * perLoop << "/loop";
	if (mode == 2)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		double us = (freq.QuadPart && calls > 0)
			? (double)ticks * 1e6 / (double)freq.QuadPart / (double)calls : 0.0;
		ss << " scan=" << us << "us";
	}
	return ss.str();
}

static bool Resolve(HMODULE ogre, const char* symbol, void** out)
{
	*out = (void*)GetProcAddress(ogre, symbol);
	if (*out)
		return true;
	LogMsg(std::string("Render: oldAnimSkip: OgreMain export missing: ") + symbol);
	return false;
}

bool InstallOldAnimLever()
{
	if (s_tried)
		return s_installed;
	s_tried = true;
	HMODULE ogre = GetModuleHandleA(s_site.module);
	if (!ogre)
	{
		LogMsg("Render: oldAnimSkip: OgreMain not loaded");
		return false;
	}
	if (!Resolve(ogre, "?FACTORY_TYPE_NAME@EntityFactory@Ogre@@2V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@A",
	             (void**)&s_entityType) ||
	    !Resolve(ogre, "?MurmurHash3_x86_32@@YAXPEBXHIPEAX@Z", (void**)&s_murmur) ||
	    !Resolve(ogre, "??9Matrix4@Ogre@@QEBA_NAEBV01@@Z", (void**)&s_matrixNe) ||
	    !Resolve(ogre, "?updateNode@TagPoint@Ogre@@QEBAXXZ", (void**)&s_tagUpdateNode) ||
	    !Resolve(ogre, "?_fireFrameListenerPostAnimationThreads@Root@Ogre@@QEAAXXZ", (void**)&s_firePostAnim) ||
	    !Resolve(ogre, "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA", (void**)&s_rootSingleton))
		return false;
	s_entityHash = TypeHash(s_entityType);
	if (!InstallModuleHook(s_site, (void*)hook_UpdateAllOldAnimations, (void**)&s_orig, NULL))
		return false;
	s_installed = true;
	return true;
}
