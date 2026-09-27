// zone_life_internal.h - Private lifecycle records, outcomes, state and helpers.
// Main thread only. Shared state has one definition in zone_life.cpp.

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

extern ZoneLifeRecord g_zl[ZONE_GRID_COUNT];
extern int  g_zlOrphans;
extern long g_zlRelSetAB;
extern long g_zlRelGone;
extern const unsigned int ZL_PLAYER_CAP;
extern const double ZL_SPACING_SEC;
extern int    g_zlLastDefer;
extern int    g_zlLastOutcome;
extern long   g_zlZombieUnloads;
extern double g_zlLastUnloadSec;
extern long   g_zlZombieClear;
extern int    g_zlZombieRetryCell;
extern unsigned char g_zlRetain[ZONE_GRID_COUNT];
extern long   g_zlReload;
extern long   g_zlReloadMod;
extern int            g_zlRecentCount;
extern long   g_ftCleared;
extern long   g_ftLate;
extern long   g_ftKeptLate;
extern unsigned char g_ftLateCell[ZONE_GRID_COUNT];
extern unsigned char g_ftKeptLateCell[ZONE_GRID_COUNT];

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
void ZlClearStep3State();
void ZlZombieFastRetry(void* zoneMgr, double now);
void ZlCheckReloads(void* zoneMgr, double now);

} // namespace

#endif // KENSHI_ZONE_OPT_ZONE_LIFE_INTERNAL_H
