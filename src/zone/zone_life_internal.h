// zone_life_internal.h - Private lifecycle records, outcomes, state and helpers.
// Main thread only. Each shared group has one definition owner.

#ifndef KENSHI_ZONE_OPT_ZONE_LIFE_INTERNAL_H
#define KENSHI_ZONE_OPT_ZONE_LIFE_INTERNAL_H

#include "zone/preload/preload_internal.h"

namespace zone_life_detail {

struct ZoneLifeRecord
{
	unsigned char flags;        // 0 = no record
	double firstSeen;           // ElapsedSec at the record's creation
	double lastInRadius;        // last time inside the retention set (retention refresh)
	double orphanSince;         // first ZoneLeak walk that found it untracked; < 0 = not
	double nextTry;             // earliest retry after a per-zone deferral (idle pass, zombie handler)
	double drainWaitSince;      // first unload attempt blocked on section drain; < 0 = none
};
enum ZlDeferKind
{
	ZLD_JOB = 0,
	ZLD_CLAIM,
	ZLD_PJ,
	ZLD_STATE,
	ZLD_COUNT
};
enum ZlOutcome
{
	ZLO_UNLOADED = 0,
	ZLO_DEFERRED,       // g_zlLastDefer says why
	ZLO_RELEASED,       // no longer the mod's to unload (gone, or in Set A/B)
	ZLO_ANOMALY,        // flags without a content, or a content the call left in place
	ZLO_UNAVAILABLE     // the unload protocol can never pass here
};

const unsigned int ZL_PLAYER_CAP = 1024;
const double ZL_SPACING_SEC        = 2.0;    // at least this long between two unloads

// Lifecycle record table, defined in zone_life.cpp.
extern ZoneLifeRecord g_zl[ZONE_GRID_COUNT];

// ZoneLeak session totals, defined in zone_life.cpp: never reset;
// g_zlOrphans is reset to -1 at load.
extern int  g_zlOrphans;
extern long g_zlRelSetAB;
extern long g_zlRelGone;
extern long   g_zlZombieUnloads;
extern long   g_zlZombieClear;
extern long   g_zlReload;
extern long   g_zlReloadMod;
extern long   g_ftCleared;
extern long   g_ftLate;
extern long   g_ftKeptLate;

// Unload out-channel, defined in zone_unload.cpp; written only by UnloadModZone.
extern int    g_zlLastDefer;
extern int    g_zlLastOutcome;
extern double g_zlLastUnloadSec;

void ZlNotePlayerCap(unsigned int count);
void ZlRelease(int cell);
const char* ZlUnloadUnavailable();
void ZlNoteUnloaded(int cell, double now);
bool UnloadModZone(void* zm, void* zone, bool save, const char* why);
void ZlAppendStep2Tokens(std::ostringstream& ss);
bool ZlBuildRetention(void* zoneMgr);
void ZlAppendStep3Tokens(std::ostringstream& ss);
void ZoneLeakReport(void* zoneMgr, double now);
void ZoneLifeUnloadPass(void* zoneMgr, double now);
void ZlClearEvictState();
void ZlClearStep3State();
void ZlClearFirstTimeState();
void ZlZombieFastRetry(void* zoneMgr, double now);
void ZlCheckReloads(void* zoneMgr, double now);

} // namespace

#endif // KENSHI_ZONE_OPT_ZONE_LIFE_INTERNAL_H
