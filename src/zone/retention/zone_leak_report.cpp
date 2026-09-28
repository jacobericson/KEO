// zone_leak_report.cpp - Lifecycle retention walk and leak diagnostics.
// Main thread: refreshes retention, releases records and empty-cell fence holds,
// stamps record ages and then logs. Takes no mod lock of its own.

#include "zone/zone_life.h"
#include "zone/zone_life_internal.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/reset/zone_reset_fence.h"
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX only; the function is resolved at runtime

using namespace zone_life_detail;

// The retention map is shared state in zone_life.cpp. The builder in
// zone_unload.cpp records whether its last rebuild read every anchor.
// This reporter consumes that result and refreshes record ages; unreadable
// anchors keep every zone retained rather than making the zero bits mean far.
// Process private bytes (PROCESS_MEMORY_COUNTERS_EX::PrivateUsage) in MB, or
// -1. K32GetProcessMemoryInfo is a kernel32 export on Windows 7+, resolved at
// runtime so the link stays KenshiLib.lib + user32.lib (no psapi.lib).
static long ZlPrivateMB()
{
	typedef BOOL (WINAPI *K32GetProcessMemoryInfo_t)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
	static K32GetProcessMemoryInfo_t s_fn = NULL;
	static bool s_tried = false;
	if (!s_tried)
	{
		s_tried = true;
		HMODULE k32 = GetModuleHandleA("kernel32.dll");
		if (k32)
			s_fn = (K32GetProcessMemoryInfo_t)GetProcAddress(k32, "K32GetProcessMemoryInfo");
	}
	if (!s_fn)
		return -1;
	PROCESS_MEMORY_COUNTERS_EX pmc;
	memset(&pmc, 0, sizeof(pmc));
	pmc.cb = sizeof(pmc);
	if (!s_fn(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc)))
		return -1;
	return (long)(pmc.PrivateUsage / (1024 * 1024));
}



namespace zone_life_detail {

// One walk over the 4096 ZoneMaps (content pointer and flags: static game
// memory, plain reads) plus a Set A/B test for each zone with a content.
// Classes, for a zone with a content outside Set A/B:
//   tracked      in the working table, or (with zoneLifeUnload on) held
//                by a record the unload pass owns;
//   orphan176/177/00  not tracked, but recorded as the mod's: +176 set /
//                +177 set / neither (reg= and proc= split orphan176 by the
//                record's registered and processed flags);
//   unexplained  not tracked and never recorded (expect 0).
// Records of zones the game adopted (Set A/B) or that are no longer loaded
// are released here too.
void ZoneLeakReport(void* zoneMgr, double now)
{
	static unsigned char inTable[ZONE_GRID_COUNT];
	memset(inTable, 0, sizeof(inTable));
	for (int i = 0; i < numPreloaded; ++i)
	{
		if (!preloadedZones[i].zoneEntry)
			continue;
		int cell = ZoneCell(preloadedZones[i].gridX, preloadedZones[i].gridY);
		if (cell >= 0)
			inTable[cell] = 1;
	}

	bool retainOk = ZlBuildRetention(zoneMgr);
	// A record owns its zone only while the idle pass can unload it: with the
	// protocol unavailable such zones are orphans again.
	bool recordOwns = zone::g_zoneCfg.zoneLifeUnloadEnabled && !ZlUnloadUnavailable();

	int nContent = 0, nSetA = 0, nSetB = 0, nTracked = 0;
	int o176 = 0, o176reg = 0, o176proc = 0, o177 = 0, o00 = 0;
	int unexplained = 0, nFar = 0, nullFlags = 0, uvHold = 0;
	unsigned resetGen = ZoneResetFenceGeneration();
	double oldest = -1.0;
	double farOldest = -1.0;   // oldest recorded zone outside retention

	for (int gx = 0; gx <= ZONE_GRID_MAX; ++gx)
	{
		for (int gy = 0; gy <= ZONE_GRID_MAX; ++gy)
		{
			void* ze = GetZoneEntry(zoneMgr, gx, gy);
			if (!ze)
				continue;
			int cell = ZoneCell(gx, gy);
			void* content = *(void**)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap_mapContent, OFF_ZONE_CONTENT));
			bool loading = IsZoneLoading(ze);
			bool access  = IsZoneAccessible(ze);
			unsigned char f = g_zl[cell].flags;

			if (!content)
			{
				// A retirement an earlier reset could not carry out is
				// answered the moment the cell is seen empty: whatever freed
				// it, nothing is held any more.
				ZoneResetFenceClear(gx, gy);
				if (f)
				{
					if (!loading && !access)
					{
						ZlRelease(cell);
						g_zlRelGone++;
					}
					else
					{
						// Flags without a content: deactivateZoneMap would do
						// nothing but clear the island label. Kept and
						// counted, never treated as unloaded.
						nullFlags++;
					}
				}
				continue;
			}

			nContent++;
			bool inA = ZoneInSetA(zoneMgr, ze);
			bool inB = ZoneInSetB(zoneMgr, ze);
			if (inA || inB)
			{
				if (inA) nSetA++;
				if (inB) nSetB++;
				if (f)
				{
					ZlRelease(cell);
					g_zlRelSetAB++;
				}
				continue;
			}

			if (retainOk && !ZlRetentionNear(cell))
			{
				nFar++;
				// How long the longest-idle recorded zone has been
				// outside the retention set. With the idle pass working it
				// stays near zoneLifeIdleSeconds + a few seconds; a value that
				// only grows is a zone the pass can never unload (the leak
				// check's fail signal, which orphan*= cannot give).
				if (f)
				{
					double idle = now - g_zl[cell].lastInRadius;
					if (idle > farOldest)
						farOldest = idle;
				}
			}
			else if (f)
				g_zl[cell].lastInRadius = now;   // retained, or no anchor to judge by

			if (inTable[cell] || (recordOwns && f != 0))
			{
				nTracked++;
				if (f)
					g_zl[cell].orphanSince = -1.0;
				continue;
			}
			if (!f)
			{
				// A cell a previous world's reset left loaded under a running
				// generation. It has no record here because the records went
				// with that world, but it has a cause, and unexplained= is
				// the signal for cells that do not.
				if (ZoneResetFenceHolds(gx, gy, resetGen))
					uvHold++;
				else
					unexplained++;
				continue;
			}

			if (g_zl[cell].orphanSince < 0.0)
				g_zl[cell].orphanSince = now;
			double age = now - g_zl[cell].orphanSince;
			if (age > oldest)
				oldest = age;
			if (loading)
			{
				o176++;
				if (f & ZL_REGISTERED) o176reg++;
				if (f & ZL_PROCESSED)  o176proc++;
			}
			else if (access)
				o177++;
			else
				o00++;
		}
	}

	g_zlOrphans = o176 + o177 + o00;

	long privMB = ZlPrivateMB();
	std::ostringstream ss;
	ss << "ZoneLeak: content=" << nContent
	   << " setA=" << nSetA << " setB=" << nSetB;
#if ZONEHAND_STEP >= 1
	// The set's own size, which is what the ambient spawn deficit is divided
	// by -- not setB= above, which counts recorded cells that are in it.
	ss << " setBsz=" << ZoneCycleSetBSize(zoneMgr);
#endif
	ss << " tracked=" << nTracked
	   << " orphan176=" << o176 << " reg=" << o176reg << " proc=" << o176proc
	   << " orphan177=" << o177
	   << " orphan00=" << o00
	   << " unexplained=" << unexplained
	   << " uvHold=" << uvHold << "/" << ZoneResetFenceCount()
	   << " oldest=";
	if (oldest >= 0.0) ss << (long)(oldest + 0.5) << "s";
	else               ss << "-";
	ss << " far=";
	if (retainOk) ss << nFar;
	else          ss << "-";
	ss << " farOldest=";
	if (farOldest >= 0.0) ss << (long)(farOldest + 0.5) << "s";
	else                  ss << "-";
	ss << " privMB=";
	if (privMB >= 0) ss << privMB;
	else             ss << "-";
	ss << " records=" << g_zlLive
	   << " released=" << g_zlRelSetAB << "/" << g_zlRelGone;
	if (nullFlags)
		ss << " nullFlags=" << nullFlags;
	ZlAppendStep2Tokens(ss);
	ZlAppendStep3Tokens(ss);
	LogMsg(ss.str());
}

} // namespace
using namespace zone_life_detail;
