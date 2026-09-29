// zone_first_time.cpp - First-time prediction and untouched-content handoff.
// Main thread, before processContent.

#include <cstring>
#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"

namespace zone_life_detail {
static unsigned char g_ftLateCell[ZONE_GRID_COUNT];     // cells caught late: predicted first-time from then on
static unsigned char g_ftKeptLateCell[ZONE_GRID_COUNT]; // cells counted in keptLate (once each)
}
namespace zone_life_detail {
void ZlClearFirstTimeState()
{
	memset(g_ftLateCell, 0, sizeof(g_ftLateCell));
	memset(g_ftKeptLateCell, 0, sizeof(g_ftKeptLateCell));
}
}
using namespace zone_life_detail;

// =========================================================================
// First-time zones and town cells: the handoff rule
// =========================================================================
//
// The reason: the game's phase-B spawn runs only for zones it loads itself.
// loadPhase2 (processState2's phase B) is what populates a zone the first time
// (collectBuildings 0x9FCD00 -> Town::spawnTheBarFlies, then
// Town::chooseResidents 0x936200 -> populateBuilding), and it runs only for
// zones in Set A. A zone the mod preloads (loadSingleZone, processContent)
// never enters Set A on its own, so its bar patrons and building
// residents are never created; they only trickle in later through platoon
// activation, and whatever phase B alone would have made (a shopkeeper) may
// never appear.
// That holds whether or not the mod ever unloads the zone, so the mod never
// takes ownership of a first-time zone or a town cell: it is left to the game.
//
// It also keeps the never-revert rule: the mod never unloads
// with save = false a zone the player could have changed, so every mod-owned
// zone must be saveable when the step-3 pass unloads it, and a
// first-time zone (content+0xA8 newGameFirstTimeLoaded == 1) is not: saving it
// writes the zone file, so its first-time population would never happen in
// that save.
//
// When content+0xA8 means something (IDB):
//   - ZoneMapContent's ctor 0xA00720 writes 1; ZoneMap::_activate 0xA0D6A0
//     (our loadSingleZone) creates the content and arms activationFlag.
//   - Only ZoneMapContent::_activate 0x9FEC00 rewrites it: 0x9FEF22
//     = !GameDataContainer::load(<SaveFileSystem::readFile("zone/zone.X.Y.zone")>),
//     0x9FF147 = 1 when checkForRepopulateTown says a town listed in
//     townsByZone[x][y] repopulates, 0x9FF508 = 1 when 0-buildinglist carries
//     an "imported" key; then loaded = 1 at 0x9FF6D1.
//   - _activate is reached only from ZoneMapContent::update 0x9FF730 (vtable
//     +32, our CallProcessContent), and only once the terrain collision is
//     loaded. So before the mod's processContent the byte is the ctor's 1,
//     and it is valid exactly when loaded == 1.
// The decision is therefore made before processContent with an equivalent
// (ZlPredictFirstTime): the save has no zone file (SaveFileSystem::fileExists
// 0x470A70 on the key checkZoneFiles 0xA0B020 builds, the same map lookup
// readFile makes; GameDataContainer::load 0x6C0800 returns 0 when the file
// does not open) or the cell lists a town (the repopulate source; also
// collectBuildings' barfly source). The "imported" key and an unreadable zone
// file cannot be seen before the finalize: after processContent, a zone whose
// content reads loaded == 1 && first-time == 1 is caught (late) and never
// registered.
//
// Whether a first-time zone has population work at all cannot be read before
// processContent: collectBuildings acts on buildings with a real town, and a
// building's town is assigned when the finalize instantiates it
// (RootObjectFactory::createBuilding 0x57C1E0: TownList::getNearestWithinItsRadius,
// the nearest player town for a first-time zone, the nearest town within
// 10000 units for a building with an interior). By ruling (c) every
// first-time zone is therefore given to the game: none is kept, none is
// saved, and firstTimeSaved= does not exist. The town-cell test in the
// prediction is what hands every town to the game, visited or not: its
// residents and barflies come from phase B.
//
// Always in force: the phase-B reason needs it even with zoneLifeUnload off.
// zoneLifeUnload decides only whether the idle pass unloads.
//
// One exception: a zone caught late -- it reads first-time
// only after the mod's own processContent -- can be given back only through the
// zombie handler's unload. Where the unload protocol can never pass
// (ZlUnloadUnavailable) that handler's
// fallback would clear +176 on the finalized content, and the game would
// finalize it a second time. So there the mod keeps the zone
// (ZlKeepLate: counted as the fourth ftHandoff= field, the cell marked late so
// its next load is handed off untouched by the prediction). The prediction
// paths (skip, +176 cleared untouched) stay unconditional in every build.

// Always true: the rule no longer depends on zoneLifeUnload or on the unload
// protocol's availability (above). Kept as the one place that says whether the
// rule is in force, for its four call sites and the FirstTime: diagnostic. The
// one part that still depends on the protocol is the late handoff after the
// finalize (ZlLateHandoffPossible, below the paragraph above).
bool ZlFirstTimeRuleOn()
{
	return true;
}

// The late handoff needs the zombie handler's unload; see "One exception"
// above. Main thread.
bool ZlLateHandoffPossible()
{
	return ZlUnloadUnavailable() == NULL;
}

// A zone caught late while the late handoff is impossible: the mod keeps it
// (the caller goes on to register it as usual). Counted and
// logged (DEV) once per cell, since the check repeats every frame until the
// zone registers; the cell is marked late, so its next load is handed to the
// game untouched by the prediction instead.
void ZlKeepLate(int i)
{
	int gx = preloadedZones[i].gridX;
	int gy = preloadedZones[i].gridY;
	int cell = ZoneCell(gx, gy);
	if (cell < 0 || g_ftKeptLateCell[cell])
		return;
	g_ftKeptLateCell[cell] = 1;
	g_ftLateCell[cell] = 1;
	g_ftKeptLate++;
#ifdef ZONEOPT_DEBUG
	std::ostringstream ss;
	ss << "First-time zone (" << gx << "," << gy
	   << ") caught late, kept (unload protocol unavailable)";
	LogDebug(ss.str());
#endif
}

// The repopulate and barfly source: the number of towns listed for this cell
// (TownList::townsByZone[x][y].count), or -1 when the town list is not there.
// `cell` must be a valid ZoneCell index.
static int ZlCellTownCount(int cell)
{
	uintptr_t townList = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_TOWN_LIST));
	if (!townList)
		return -1;
	// townsByZone[x][y] = element x*64 + y (collectBuildings: y + (x << 6)),
	// the ZoneCell index; lektor<hand> is 24 bytes (klib_layout.cpp S(HandLektor,24)).
	const uintptr_t HAND_LEKTOR_SIZE = 24;
	uintptr_t byZone = KLIB_MEMBER(2, townList, TownList_townsByZone, OFF_TOWNLIST_BY_ZONE);
	uintptr_t list = byZone + (uintptr_t)cell * HAND_LEKTOR_SIZE;
	return (int)*(unsigned int*)(KLIB_MEMBER(2, list, HandLektor_count, 8));
}

// The zone file, by the exact key ZoneManager::checkZoneFiles builds:
// 1 = the save has it, 0 = it does not, -1 = cannot be told.
static int ZlZoneFileExists(int gx, int gy)
{
	if (!fn_sfsGetSingleton || !fn_sfsFileExists)
		return -1;
	void* sfs = fn_sfsGetSingleton();
	if (!sfs)
		return -1;
	std::ostringstream key;
	key << "zone/zone." << gx << "." << gy << ".zone";
	std::string name = key.str();
	return fn_sfsFileExists(sfs, &name) ? 1 : 0;
}

// 1 = the zone's finalize will read it as first-time (or it cannot be told:
// treated the same), 0 = it will read the save's zone file. Main thread
// (SaveFileSystem::fileExists takes no lock; ZoneMapContent::_activate calls
// readFile on the main thread the same way).
int ZlPredictFirstTime(int gx, int gy)
{
	int cell = ZoneCell(gx, gy);
	if (cell < 0)
		return 1;
	if (g_ftLateCell[cell])
		return 1;

	// Any town listed for this cell (or no town list to ask): first-time.
	if (ZlCellTownCount(cell) != 0)
		return 1;

	return ZlZoneFileExists(gx, gy) == 1 ? 0 : 1;
}

#ifdef ZONEOPT_DEBUG
// Tests the premise "does a first-time town the mod preloads get its
// residents and barflies?", DEV only, one line per cell per session: once the
// mod's own processContent has finalized a zone's content (loaded == 1), log
// what the game will have decided (content+0xA8) beside the two inputs the
// prediction reads, and whether the mod keeps the zone registered (kept=1) or
// the first-time rule gives it to the game here (kept=0). The rule is
// unconditional, zoneLifeUnload=false included (the game's
// phase-B spawn runs only for zones it loads itself), so kept=1 only for a
// zone that reads not-first-time after the finalize, or one caught late where
// the unload protocol can never pass (kept). A zone the
// prediction already gave to the game never reaches this line.
void ZlLogFirstTimeDiag(int i, void* content)
{
	static unsigned char logged[ZONE_GRID_COUNT] = { 0 };
	int gx = preloadedZones[i].gridX;
	int gy = preloadedZones[i].gridY;
	int cell = ZoneCell(gx, gy);
	if (cell < 0 || logged[cell] || !preloadedZones[i].contentProcessed)
		return;
	int ft = 0, ld = 0, act = 0;
	if (!ReadContentLifeFlags(content, &ft, &ld, &act) || ld != 1)
		return;   // not finalized yet: the line waits for loaded == 1
	logged[cell] = 1;
	int kept = (ZlFirstTimeRuleOn() && ft != 0 && ZlLateHandoffPossible()) ? 0 : 1;   // the block below the call
	std::ostringstream ss;
	ss << "FirstTime: zone(" << gx << "," << gy << ") ft=" << ft
	   << " towns=" << ZlCellTownCount(cell)
	   << " file=" << ZlZoneFileExists(gx, gy)
	   << " kept=" << kept;
	LogDebug(ss.str());
}
#endif

// A zone that never gets a mod processContent: hand it back untouched.
// Returns true when the table entry was dropped (the caller moves on).
//   in Set A or B: the game is loading it or owns it: drop the entry;
//   untouched content (activationFlag armed, loaded 0): the pipeline handoff's step,
//     +176 cleared; ZoneMap::_activate reuses the content when the game loads
//     the cell (0xA0D7BF) and loadPhase2 finalizes and populates it. The
//     record stays (ZL_HANDOFF): with zoneLifeUnload on, the idle pass unloads
//     it (save = false, never accessible, nothing written) if the game never
//     takes it; with it off the zone simply stays as the pipeline handoff leaves one;
//   otherwise (finalized by someone else, never ours to keep): left
//     pending = false, so the zombie handler unloads it with save = false.
//     Where the unload protocol can never pass, the handler's fallback
//     would clear +176 on that finalized content instead, so the post-finalize
//     late call is not made there (ZlKeepLate).
bool ZlGiveToGame(void* zoneMgr, int i, void* ze, void* content, const char* why)
{
	int gx = preloadedZones[i].gridX;
	int gy = preloadedZones[i].gridY;
	const char* what;
	bool dropped;
	if (ZoneInSetA(zoneMgr, ze) || ZoneInSetB(zoneMgr, ze))
	{
		DropTrackedZone(i);
		what = "game's";
		dropped = true;
	}
	else if (ContentUntouched(content))
	{
		*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_stateT_mainThreadData__zoneBeingLoaded, OFF_ZONE_IS_LOADING)) = 0;
		ZlTouchZone(ze, ZL_HANDOFF);
		DropTrackedZone(i);
		g_ftCleared++;
		what = "+176 cleared";
		dropped = true;
	}
	else
	{
		if (preloadedZones[i].pending && pendingCount > 0)
			pendingCount--;
		preloadedZones[i].pending = false;
		int cell = ZoneCell(gx, gy);
		if (cell >= 0)
			g_ftLateCell[cell] = 1;
		g_ftLate++;
		what = "left to the zombie unload";
		dropped = false;
	}
	std::ostringstream ss;
	ss << "First-time zone (" << gx << "," << gy << ") " << why
	   << ": given to the game (" << what << ")";
	LogMsg(ss.str());
	return dropped;
}
