#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/module_hooks.h"
#include "render/ogre_purge_policy.h"
#include "zone/handoff/zone_handoff.h"
#include "zone/transition.h"
#include "base/core.h"
#include <iomanip>
#include <sstream>

// ZoneManager::processLoading's 3->4 phase branch purges Ogre's unreferenced
// resources once per loading cycle: four Ogre::ResourceGroupManager group
// unloads (General, foliage, Landscape, Overlaymaps) and, through the same
// base-class implementation, one TextureManager and one MeshManager
// unloadUnreferencedResources call. A cycle the handoff mechanism raised to
// move a cohort of prepared cells into the game's own tracking runs this
// same purge for no player-visible reason; skipping it there, with a timed
// fallback so the skip can never run forever, is this lever's whole job.
// What the purge does when it runs is untouched -- the original is called
// exactly as the game would have.

typedef void (*UnloadGroup_t)(void* mgr, const void* groupName, bool reloadableOnly);
typedef void (*UnloadUnreferenced_t)(void* mgr, bool reloadableOnly);

static const ModuleSite s_groupSite =
{
	"ResourceGroupManager::unloadUnreferencedResourcesInGroup", "OgreMain_x64.dll",
	"?unloadUnreferencedResourcesInGroup@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z",
	0,
	{ 0x48,0x8B,0xC4,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x81,0xEC,0x20,0x02 }
};

// ResourceManager's own implementation, exported once and shared by every
// resource manager that does not override it -- TextureManager and
// MeshManager both call in here for their 3->4 unload.
static const ModuleSite s_unrefSite =
{
	"ResourceManager::unloadUnreferencedResources", "OgreMain_x64.dll",
	"?unloadUnreferencedResources@ResourceManager@Ogre@@UEAAX_N@Z",
	0,
	{ 0x40,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,0x48 }
};

static UnloadGroup_t        s_origGroup = NULL;
static UnloadUnreferenced_t s_origUnref = NULL;
static bool                 s_tried     = false;
static bool                 s_installed = false;

// Main thread only, like every loadingPhase branch this lever touches. Seeded
// at install time (below) rather than left at a sentinel: a session whose
// first-ever purge call happens to be eligible would otherwise see an
// elapsed time back to program start and take that as a fallback run before
// any real purge ever ran.
static double s_lastPurgeAt = 0.0;

static volatile LONG s_calls    = 0;   // every intercepted call, skip key on or off
static volatile LONG s_skipped  = 0;
static volatile LONG s_fallback = 0;

// True only while a real transition never joined the cycle now running: a
// real transition's purge always runs, whatever the ledger says about how
// the cycle started.
static bool Eligible()
{
	return ZoneHandoffAdoptionCycleInFlight() && !isTransitionActive;
}

// One decision per intercepted call; the six calls a purge makes in one
// cycle each get their own, all reading the same clock and the same last-run
// stamp, so they agree unless the clock ticks a whole second between them.
static bool ShouldRun(double now)
{
	InterlockedIncrement(&s_calls);
	if (!g_renderCfg.adoptOgrePurgeSkip)
	{
		s_lastPurgeAt = now;
		return true;
	}
	OgrePurgeDecision d = OgrePurgeSkipDecide(Eligible(), now, s_lastPurgeAt,
	                                          (double)g_renderCfg.adoptOgrePurgeMaxSkipSeconds);
	if (d.action == OGREPURGE_SKIP)
	{
		InterlockedIncrement(&s_skipped);
		return false;
	}
	s_lastPurgeAt = now;
	if (d.countFallback)
		InterlockedIncrement(&s_fallback);
	return true;
}

static void hook_UnloadGroup(void* mgr, const void* groupName, bool reloadableOnly)
{
	if (ShouldRun(ElapsedSec()))
		s_origGroup(mgr, groupName, reloadableOnly);
}

static void hook_UnloadUnreferenced(void* mgr, bool reloadableOnly)
{
	if (ShouldRun(ElapsedSec()))
		s_origUnref(mgr, reloadableOnly);
}

bool InstallOgrePurgeSkip()
{
	if (s_tried)
		return s_installed;
	s_tried = true;
	HMODULE ogre = GetModuleHandleA(s_groupSite.module);
	if (!ogre)
	{
		LogMsg("Render: adoptOgrePurgeSkip: OgreMain not loaded");
		return false;
	}
	bool group = InstallModuleHook(s_groupSite, (void*)&hook_UnloadGroup, (void**)&s_origGroup, NULL);
	bool unref = InstallModuleHook(s_unrefSite, (void*)&hook_UnloadUnreferenced, (void**)&s_origUnref, NULL);
	s_installed = group && unref;
	if (s_installed)
		s_lastPurgeAt = ElapsedSec();
	return s_installed;
}

std::string OgrePurgeStatsToken()
{
	LONG calls    = InterlockedExchange(&s_calls, 0);
	LONG skipped  = InterlockedExchange(&s_skipped, 0);
	LONG fallback = InterlockedExchange(&s_fallback, 0);
	if (!s_installed)
		return std::string();
	std::ostringstream ss;
	ss << " ogrePurge=" << calls << "/" << skipped << "/" << fallback;
	return ss.str();
}
