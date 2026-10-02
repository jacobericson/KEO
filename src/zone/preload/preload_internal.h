// preload_internal.h - Shared preload and lifecycle declarations.
// Public declarations live in preload.h; internal preload and zone-lifecycle
// consumers use this header. Lifecycle-only private state is declared in
// zone_life_internal.h.
#ifndef KEO_PRELOAD_INTERNAL_H
#define KEO_PRELOAD_INTERNAL_H

#include "zone/preload/preload.h"
#include "game/game.h"


// One per-cell index for every per-zone table in this file: gx*64 + gy, the
// order ZoneManager::getZoneMap / lookupZone (0xA07C10) uses for the ZoneMap
// array (GetZoneEntry). The registry guard's once-per-zone log bitmap and the
// DEV preload-skip bitmap used gy*64 + gx before this indexing; both only record
// "this cell was logged", so switching them to this index changes nothing a
// log reader sees. The mapping between the two is the transpose:
// gy*64 + gx == ZoneCell(i % 64, i / 64) for i = ZoneCell(gx, gy). The game's
// own handle-registry container id (gx + 64*gy + 1, ZoneRegistrationOk) is a
// third convention and stays as the game writes it. -1 = off the grid.
static inline int ZoneCell(int gx, int gy)
{
	if (gx < 0 || gx > ZONE_GRID_MAX || gy < 0 || gy > ZONE_GRID_MAX)
		return -1;
	return gx * (ZONE_GRID_MAX + 1) + gy;
}


// =========================================================================
// Registry guard functions: zone_registry.cpp; bitmap: zone_life.cpp. Sites:
// preload_queue.cpp (adopt) and preload_prepare.cpp (register, process).
// =========================================================================
enum RegGuardSite
{
	REG_SITE_ADOPT = 0,
	REG_SITE_REGISTER,
	REG_SITE_PROCESS,
	REG_SITE_COUNT
};

extern unsigned char g_regGuardLogged[REG_SITE_COUNT][ZONE_GRID_COUNT];

bool RegistryGuardPasses(void* zoneEntry, int site);
void DropTrackedZone(int i);

// preload_queue.cpp: registration iteration order, used by preload_prepare.cpp
// as well.
int BuildPreloadOrder(int* order);

// preload_pipe_stats.cpp: preload pipeline latency instrumentation. loaded= is filled
// by preload_queue.cpp, registered= and the latency percentiles by
// preload_prepare.cpp.
static const int PIPE_SAMPLE_CAP = 64;

struct PipeSampleSet
{
	double toReadyMs[PIPE_SAMPLE_CAP];     int toReadyN;
	double to264Ms[PIPE_SAMPLE_CAP];       int to264N;
	double toRegMs[PIPE_SAMPLE_CAP];       int toRegN;
	double procContentMs[PIPE_SAMPLE_CAP]; int procContentN;
	int loaded;
	int registered;
	int readyEmpty;
	int notReadyAtCall;
	int thingsAfter0;
	int thingsAfter1;
};
extern PipeSampleSet g_pipe;

void PipeSampleAdd(double* arr, int* n, double ms);

// preload_saveload.cpp ticks these from PreloadCheckSaveLoad, the only
// preload entry camera_zone_hook.cpp calls unconditionally every main-thread frame.
void PreloadStep1Tick(void* zoneMgr);
void H15Reset();

// Zone-lifecycle record flags (records in zone_life.cpp). Read by preload_queue.cpp
// (ProcessPreloadQueue) and preload_prepare.cpp (TryRegisterPreloadedZones),
// written by preload_queue.cpp, preload_prepare.cpp and ZlGiveToGame
// (zone_first_time.cpp).
enum ZoneLifeFlag
{
	ZL_MOD_LOADED = 0x01,   // fn_loadSingleZone returned non-zero for it
	ZL_ADOPTED176 = 0x02,   // taken into the table while +176 was already set
	ZL_REGISTERED = 0x04,   // the mod called registerZoneSections on it
	ZL_PROCESSED  = 0x08,   // the mod called processContent (vtable[4]); the call,
	                        // not proof of the finalize (content+0xD8 is)
	ZL_HANDOFF    = 0x20,   // the first-time handoff cleared +176 on it
	ZL_DOUBLE     = 0x40    // finalized content was actually handed back to
	                        // the game (the double finalize can then happen):
	                        // never saved at unload. No path hands
	                        // finalized content back, so nothing sets it and the
	                        // save rule's test is a guard.
};

extern int g_zlLive;

void ZlTouchZone(void* zoneEntry, unsigned char flag);
unsigned char ZlFlagsOf(void* zoneEntry);
bool ReadContentLifeFlags(void* content, int* firstTime, int* loaded, int* activation);
bool ContentUntouched(void* content);
void ZlClearAll();

extern long g_ftSkip;   // ftHandoff=<skip>/../../..: predicted first-time, never loaded

bool ZlFirstTimeRuleOn();
bool ZlLateHandoffPossible();
int  ZlPredictFirstTime(int gx, int gy);
bool ZlGiveToGame(void* zoneMgr, int i, void* ze, void* content, const char* why);
void ZlKeepLate(int i);
#ifdef KEO_DEBUG
void ZlLogFirstTimeDiag(int i, void* content);
#endif

#endif // KEO_PRELOAD_INTERNAL_H
