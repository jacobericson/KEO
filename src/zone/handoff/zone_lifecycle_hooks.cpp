#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/handoff/zone_lifecycle_hooks.h"

#if ZONEHAND_STEP >= 2

#include "zone/handoff/zone_handoff.h"
#include "zone/retention/zone_retention.h"
#include "navmesh/nm_workers.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// ZoneManager::activateZoneMap(this, map, centre, range, type, timer). The
// third argument is an iVector2 passed whole in a register and unused by the
// body; the last two arrive on the stack.
typedef bool (__fastcall *activateZoneMap_t)(void* zoneMgr, void* map, __int64 centre,
                                             int range, int type, float deactivationTimer);
static activateZoneMap_t orig_activateZoneMap = NULL;

// Two responsibilities, one detour, because every native activation in the
// image arrives here:
//
//   * a town refresh carrying a nonpositive timer is refused outright. The
//     value is the largest camera/player countdown over the town's coverage,
//     so a nonpositive one means nothing in that coverage is leased and the
//     engine would substitute a fixed default, renewing the whole coverage
//     from nothing, every frame, forever;
//   * a cell the mod is holding privately is taken over. The engine's own
//     activate refuses such a cell and returns 0, which leaves it outside
//     the pending set and so outside the game's reach; inserting it and
//     reporting success hands it back, and the caller's own scheduling
//     follows unchanged.
//
// The takeover inserts into the same set, through the same helper, that the
// original would have used on this very call, so it is safe exactly where
// the original is.
static bool __fastcall hook_activateZoneMap(void* zoneMgr, void* map, __int64 centre,
                                            int range, int type, float deactivationTimer)
{
	if (ZoneHandoffTownGuardRefuses(type, deactivationTimer))
		return false;

	bool activated = orig_activateZoneMap(zoneMgr, map, centre, range, type, deactivationTimer);

	// The original's answer is true when any cell in its range succeeded, so
	// it cannot say whether this one did. The takeover decides from the
	// cell's own flags instead.
	if (map && ZoneHandoffTakeover(zoneMgr, map, type))
		activated = true;

	return activated;
}

#if ZONEHAND_STEP >= 3

// ZoneMap::update(ZoneMap*) -> false when the cell expired and left the
// active set.
typedef bool (__fastcall *zoneMapUpdate_t)(void* zoneEntry);
static zoneMapUpdate_t orig_zoneMapUpdate = NULL;

// The original decides every cell's fate; this only decides whether the
// countdown it is about to read has run out. Holding is a write to the town
// countdown and nothing else, so the original's own expiry branch does every
// teardown there is — the deactivate, the set erase and the notification —
// and it does them inside the fences when the policy wants the cell gone.
//
// Runs for every member of the active set, every frame, so a cell that is
// not about to expire costs four reads and the call.
static bool __fastcall hook_zoneMapUpdate(void* zoneEntry)
{
	if (!ZoneRetentionWantsRelease(zoneEntry))
		return orig_zoneMapUpdate(zoneEntry);

	// The zone's navmesh goes with it, so no mod navmesh thread may be
	// working on the cell and none may start while the teardown runs. This
	// call blocks briefly on the generator's queue mutex, which is why the
	// decision above lets at most one cell per frame reach it.
	NavMeshUnloadBegin ub = NavMeshBeginZoneUnload(zoneEntry);
	if (ub == NM_UL_UNAVAILABLE)
	{
		// No such fence exists in this build or this session, and none ever
		// will. Holding on that would hold every cell forever, so the cell
		// expires with vanilla's own exposure, which is what it has today.
		bool kept = orig_zoneMapUpdate(zoneEntry);
		ZoneRetentionNoteReleased(zoneEntry, !kept, false);
		return kept;
	}
	if (ub != NM_UL_BEGUN)
	{
		ZoneRetentionHoldInstead(zoneEntry, ZONE_RETENTION_DEFER_NAV);
		return orig_zoneMapUpdate(zoneEntry);
	}

	// Zero wait: a blocking acquisition here would stall the frame for as
	// long as a generation holds the lock, once per cell.
	NavMeshPjLockResult pj = NavMeshTryLockProcessJobFor(0, NULL);
	if (pj == NM_PJLOCK_TIMEOUT)
	{
		NavMeshEndZoneUnload();
		// The generation entry points stand aside for a couple of seconds, so
		// one of the next frames wins the lock.
		NavMeshRequestPjPriority();
		ZoneRetentionHoldInstead(zoneEntry, ZONE_RETENTION_DEFER_PJ);
		return orig_zoneMapUpdate(zoneEntry);
	}

	bool kept;
	{
		NavMeshUnloadFenceScope scope(pj == NM_PJLOCK_HELD);
		kept = orig_zoneMapUpdate(zoneEntry);
	}
	ZoneRetentionNoteReleased(zoneEntry, !kept, true);
	return kept;
}

static void InstallZoneMapUpdateHook(int* installed)
{
	if (HookInstallRow(HOOK_ZONEMAP_UPDATE, hook_zoneMapUpdate, (void**)&orig_zoneMapUpdate,
	                   installed, true) == NULL)
	{
		LogMsg(std::string("Zone lifecycle: ZoneMap::update detour installed (retention=")
		       + (zone::g_zoneCfg.zoneRetentionEnabled ? "on)" : "off)"));
		return;
	}
	orig_zoneMapUpdate = NULL;
	ErrorLog("Zone lifecycle: ZoneMap::update detour NOT installed — nothing holds a cell the game has "
	         "taken over from the mod, so every one of them expires on the first pass that reaches it; "
	         "this build prepares cells that are then thrown away");
}

#endif // ZONEHAND_STEP >= 3

void InstallZoneLifecycleHooks(int* installed, int*)
{
	ZoneHandoffInit();

	if (HookInstallRow(HOOK_ACTIVATE_ZONEMAP, hook_activateZoneMap, (void**)&orig_activateZoneMap,
	                   installed, true) == NULL)
	{
		LogMsg(std::string("Zone lifecycle: activateZoneMap detour installed (townGuard=")
		       + (zone::g_zoneCfg.townGuardEnabled ? "on)" : "off)"));
	}
	else
	{
		orig_activateZoneMap = NULL;
		ErrorLog("Zone lifecycle: activateZoneMap detour NOT installed — this build publishes no zone itself, "
		         "so every cell the mod prepares would stay out of the game's reach");
	}

#if ZONEHAND_STEP >= 3
	ZoneRetentionInit();
	InstallZoneMapUpdateHook(installed);
#endif
}

#else

void InstallZoneLifecycleHooks(int*, int*) {}

#endif // ZONEHAND_STEP >= 2
