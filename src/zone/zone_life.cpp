// zone_life.cpp - Lifecycle records, shared state, main-thread tick and reset.
// The three public preload globals keep global linkage; private state and
// cross-file helpers use zone_life_detail. All lifecycle mutations run on main.

#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"

using namespace zone_life_detail;
#include "zone/transition.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/preload/preload_internal.h"
#include "zone/handoff/zone_handoff.h"


unsigned char g_regGuardLogged[REG_SITE_COUNT][ZONE_GRID_COUNT];
// =========================================================================
// Zone-lifecycle record
// =========================================================================
//
// One record per grid cell (ZoneCell index) for every zone the mod loads,
// adopts at +176, registers, processes or hands off. Unlike the
// working table (preloadedZones), transition end never clears it: only the
// save-load reset and startup do (ClearPreloadStateImpl), plus two releases:
//   - the zone shows up in Set A or Set B: the game owns it from then on
//     (ZoneInSetA/B, game.cpp: a mod zone with +176 or +177 set can
//     never enter either, so this is a zone whose flags were cleared, e.g. a
//     handoff zone the game adopted);
//   - the mod unloads it (UnloadModZone), or it is gone
//     (content NULL and both flags clear: nothing is loaded any more).
// Main thread only, every read and every write. ZoneLifeFlag is declared in
// preload_internal.h: preload_queue.cpp and preload_prepare.cpp touch it too.


namespace zone_life_detail {

ZoneLifeRecord g_zl[ZONE_GRID_COUNT];

} // namespace
using namespace zone_life_detail;

int  g_zlLive        = 0;    // records with flags != 0

namespace zone_life_detail {

int  g_zlOrphans     = -1;   // last ZoneLeak walk's orphan total (-1 = none yet)
long g_zlRelSetAB    = 0;    // records released because the game adopted the zone
long g_zlRelGone     = 0;    // records released because the zone was no longer loaded

} // namespace
using namespace zone_life_detail;


// Players counted for the zone-lifecycle anchors: the list is the game's own
// lektor, so the cap only guards a garbage count;
// large late-game factions passed the old 200, which turned every unload off
// without a word. A count above it is logged once.


static bool g_zlPlayerCapLogged = false;

namespace zone_life_detail {

void ZlNotePlayerCap(unsigned int count)
{
	if (count <= ZL_PLAYER_CAP || g_zlPlayerCapLogged)
		return;
	g_zlPlayerCapLogged = true;
	std::ostringstream ss;
	ss << "ZoneLife: player list count " << count << " above " << ZL_PLAYER_CAP
	   << "; anchors unreadable, no idle unloads while it lasts";
	LogMsg(ss.str());
}

} // namespace
using namespace zone_life_detail;


static void ZlResetRecord(int cell)
{
	g_zl[cell].flags          = 0;
	g_zl[cell].firstSeen      = 0.0;
	g_zl[cell].lastInRadius   = 0.0;
	g_zl[cell].orphanSince    = -1.0;
	g_zl[cell].nextTry        = 0.0;
	g_zl[cell].drainWaitSince = -1.0;
}

static void ZlTouchCell(int cell, unsigned char flag)
{
	if (cell < 0)
		return;
	if (g_zl[cell].flags == 0)
	{
		double now = ElapsedSec();
		ZlResetRecord(cell);
		g_zl[cell].firstSeen    = now;
		g_zl[cell].lastInRadius = now;
		g_zlLive++;
	}
	g_zl[cell].flags |= flag;
}

void ZlTouchZone(void* zoneEntry, unsigned char flag)
{
	if (!zoneEntry)
		return;
	ZlTouchCell(ZoneCell(GetZoneGridX(zoneEntry), GetZoneGridY(zoneEntry)), flag);
}

unsigned char ZlFlagsOf(void* zoneEntry)
{
	if (!zoneEntry)
		return 0;
	int cell = ZoneCell(GetZoneGridX(zoneEntry), GetZoneGridY(zoneEntry));
	return (cell >= 0) ? g_zl[cell].flags : 0;
}


namespace zone_life_detail {

void ZlRelease(int cell)
{
	if (cell < 0 || g_zl[cell].flags == 0)
		return;
	ZlResetRecord(cell);
	if (g_zlLive > 0)
		g_zlLive--;
}

} // namespace
using namespace zone_life_detail;


// ZoneMapContent fields this file reads (typed IDB get_type ZoneMapContent),
// pinned by klib_member_fields.inc rows:
//   +0xA8  bool newGameFirstTimeLoaded: 1 from the ctor (0xA00720); rewritten
//          by ZoneMapContent::_activate (0x9FEC00) before loaded is set, so it
//          means something only once loaded == 1
//   +0xD8  bool loaded: 1 after the first finalize (written under mutex +0xE0)
// activationFlag (+0x108) goes through KLIB_MEMBER as elsewhere in this file.
static const size_t OFF_ZMC_FIRST_TIME = 0xA8;
static const size_t OFF_ZMC_LOADED     = 0xD8;
KLIB_ASSERT_OFFSET(ZoneMapContent_newGameFirstTimeLoaded, OFF_ZMC_FIRST_TIME);
KLIB_ASSERT_OFFSET(ZoneMapContent_loaded, OFF_ZMC_LOADED);

// The three content bytes in one SEH-guarded read (POD only here, no C++
// object needs unwinding). false = the read faulted; the outputs are then 0.
// The content pointer comes from the zone on the main thread, the only thread
// that deletes a content, so the fault case is a backstop.
bool ReadContentLifeFlags(void* content, int* firstTime, int* loaded, int* activation)
{
	*firstTime  = 0;
	*loaded     = 0;
	*activation = 0;
	if (!content)
		return false;
	bool ok = false;
	GuardEnter();
	__try
	{
		*firstTime  = *(volatile unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_newGameFirstTimeLoaded, OFF_ZMC_FIRST_TIME));
		*loaded     = *(volatile unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_loaded, OFF_ZMC_LOADED));
		*activation = *(volatile unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG));
		ok = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*firstTime  = 0;
		*loaded     = 0;
		*activation = 0;
		ok = false;
	}
	GuardLeave();
	return ok;
}

// "Untouched" content in the game's own terms: the finalize
// (ZoneMapContent::_activate) has not run, so handing the zone to the game
// finalizes it exactly once. activationFlag still armed and loaded still 0.
// The mod's contentProcessed flag is not evidence: a processContent call made
// before the terrain collision loaded finalizes nothing.
bool ContentUntouched(void* content)
{
	int firstTime = 0, loaded = 0, activation = 0;
	if (!ReadContentLifeFlags(content, &firstTime, &loaded, &activation))
		return false;
	return activation != 0 && loaded == 0;
}

// Startup / save-load reset (ClearPreloadStateImpl, preload_saveload.cpp).
// Everything the record names dies with the old world. Declared in
// preload_internal.h.



namespace zone_life_detail {

long   g_zlZombieUnloads     = 0;     // zombieUl=
long   g_zlZombieClear       = 0;     // zombieClr=: +176 clears while the unload is unavailable

} // namespace
using namespace zone_life_detail;

static const double ZONELEAK_INTERVAL = 30.0;   // seconds between ZoneLeak: lines


static double g_zlNextLeak = 0.0;
void ZoneLifeTick(void* zoneMgr, double now)
{
	if (!zoneMgr || !IsMainThread())
		return;
	// Never during a save load (the caller returns first, this is the backstop).
	if (*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)zoneMgr, ZoneManager_justLoadedAGame, OFF_ZM_LOADING)) != 0)
		return;

	// A zombie that lost the zero-wait try goes first, every frame
	// while the priority request stands (zombies before idle candidates).
	ZlZombieFastRetry(zoneMgr, now);
	// Not in a frame that has already admitted a cohort: both start a
	// loading cycle's worth of work, and the pass's own spacing bounds how
	// often it runs, not what it runs alongside.
	if (zone::g_zoneCfg.zoneLifeUnloadEnabled && !ZoneHandoffAdoptedThisFrame())
		ZoneLifeUnloadPass(zoneMgr, now);

	if (isTransitionActive || InterlockedCompareExchange(&transitionEndPending, 0, 0) != 0)
		return;
	if (g_zlNextLeak <= 0.0)
		g_zlNextLeak = now + ZONELEAK_INTERVAL;
	if (now >= g_zlNextLeak)
	{
		ZoneLeakReport(zoneMgr, now);
		g_zlNextLeak = now + ZONELEAK_INTERVAL;
	}
	ZoneCycleSampleLeases(zoneMgr, now);
}

void ZlClearAll()
{
	for (int c = 0; c < ZONE_GRID_COUNT; ++c)
		ZlResetRecord(c);
	g_zlLive    = 0;
	g_zlOrphans = -1;
	ZlClearEvictState();
	ZlClearStep3State();
	ZlClearFirstTimeState();
}



namespace zone_life_detail {

long   g_zlReload        = 0;       // zlReload=<n>/..: content back within 60 s of our unload
long   g_zlReloadMod     = 0;       //          ../<m>: of those, the mod has it again

} // namespace
using namespace zone_life_detail;

long   g_ftSkip          = 0;       // ftHandoff=<skip>/../../..: predicted first-time, never loaded

namespace zone_life_detail {

long   g_ftCleared       = 0;       //   ../<cleared>/../..: loaded, +176 cleared untouched (the P3 step)
long   g_ftLate          = 0;       //   ../../<late>/..: read first-time after the finalize, left to the zombie unload
long   g_ftKeptLate      = 0;       //   ../../../<keptLate>: caught late with the unload unavailable, kept (cells)

} // namespace
using namespace zone_life_detail;
