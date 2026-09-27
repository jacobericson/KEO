// zone_registry.cpp - Zone handle-registration guard and tracking drops.
// Main thread only; reads slots, records refusals and drops tracked entries.

#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"
#include "zone/handoff/zone_handoff.h"

using namespace zone_life_detail;

// =========================================================================
// Registry guard (defence in depth)
// =========================================================================
//
// Every handle issued for a zone's objects resolves through the zone's slot in
// the ZoneMap handle registry (game.h, RVA_ZONEMAP_HANDLE_*): slot
// gx + 64*gy + 1 must hold this content's HandleDummy. The game's save-load
// reset wipes the registry after unloading only Set A and Set B, so a zone the
// mod loaded before a load could keep a content whose slot is gone, and every
// object the game then instantiates in it resolves to NULL (a crash in
// Building::createPhysical). hook_resetUnloadZones in preload_saveload.cpp
// removes those zones at the reset; this check is the backstop on every path
// that adopts a zone or hands its content to the game, and regSkip= says
// whether it ever had to act.
//
// Main thread only (every caller is). Plain reads; the slot array only grows
// or is rewritten by main-thread code (content ctor, prepareUnload, the reset).
// RegGuardSite is declared in preload_internal.h: the three sites are called
// from preload_queue.cpp (adopt) and preload_prepare.cpp (register, process).

static const char* const kRegSiteName[REG_SITE_COUNT] =
	{ "adopt", "register", "process" };

// One PROD line per zone per site until the next load: the adopt site in
// particular would otherwise log every time the preload queue revisits the
// same orphan. regSkipCount still counts every refusal.

// True when the zone's content is non-NULL, its registry index is inside the
// slot array, and the slot holds this content's HandleDummy. *slotOut and
// *dummyOut receive what was read (NULL where the walk stopped first).
static bool ZoneRegistrationOk(void* zoneEntry, void** slotOut, void** dummyOut)
{
	*slotOut  = NULL;
	*dummyOut = NULL;
	if (!zoneEntry)
		return false;

	void* content = *(void**)(KLIB_MEMBER(2, (uintptr_t)zoneEntry, ZoneMap_mapContent, OFF_ZONE_CONTENT));
	if (!content)
		return false;

	void* dummy = *(void**)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_handleDummy, OFF_ZMC_HANDLE_DUMMY));
	*dummyOut = dummy;
	if (!dummy)
		return false;

	// Same fields and order as the content ctor's container id (0xA008C8):
	// ZoneMap+24 + (ZoneMap+28 << 6) + 1.
	int gx = GetZoneGridX(zoneEntry);
	int gy = GetZoneGridY(zoneEntry);
	if (gx < 0 || gx > ZONE_GRID_MAX || gy < 0 || gy > ZONE_GRID_MAX)
		return false;
	unsigned int idx = (unsigned int)(gx + 64 * gy + 1);

	void** slots = *(void***)(gameBase + RVA_ZONEMAP_HANDLE_SLOTS);
	unsigned int count = *(unsigned int*)(gameBase + RVA_ZONEMAP_HANDLE_COUNT);
	if (!slots || idx >= count)
		return false;

	void* slot = slots[idx];
	*slotOut = slot;
	return slot == dummy;
}

// Returns true when the zone may be handed to the game. On a refusal it counts
// and logs; the caller does not call into the game for this zone and drops it
// from the mod's tracking.
bool RegistryGuardPasses(void* zoneEntry, int site)
{
	void* slot  = NULL;
	void* dummy = NULL;
	if (ZoneRegistrationOk(zoneEntry, &slot, &dummy))
		return true;

	regSkipCount++;

	int gx = zoneEntry ? GetZoneGridX(zoneEntry) : -1;
	int gy = zoneEntry ? GetZoneGridY(zoneEntry) : -1;
	int cell = ZoneCell(gx, gy);
	if (cell < 0 || !g_regGuardLogged[site][cell])
	{
		if (cell >= 0)
			g_regGuardLogged[site][cell] = 1;
		std::ostringstream ss;
		ss << "Registry guard: zone (" << gx << "," << gy << ") regOk=0 at "
		   << kRegSiteName[site] << " slot=" << slot << " dummy=" << dummy
		   << " \xE2\x80\x94 skipped";
		LogMsg(ss.str());
	}
	return false;
}

// Drop a tracked zone after a guard refusal. The slot is left in place with a
// NULL zoneEntry and no stage flags, so the order[] indices the caller is
// iterating stay valid; EvictStaleZones compacts NULL entries on its next run.
// Coordinates go to -1 so IsZoneQueued no longer matches it.
void DropTrackedZone(int i)
{
	ZoneHandoffNoteDropped(preloadedZones[i].gridX, preloadedZones[i].gridY);
	if (preloadedZones[i].pending && pendingCount > 0)
		pendingCount--;
	preloadedZones[i].zoneEntry        = NULL;
	preloadedZones[i].gridX            = -1;
	preloadedZones[i].gridY            = -1;
	preloadedZones[i].gameOwned        = false;
	preloadedZones[i].pending          = false;
	preloadedZones[i].registered       = false;
	preloadedZones[i].registeredEmpty  = false;
	preloadedZones[i].contentProcessed = false;
	preloadedZones[i].carried          = false;
}
