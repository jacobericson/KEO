#include "zone/preload/coverage_stats.h"

#include <windows.h>
#include <sstream>

bool CoverageFullGridAllowed(int maxPreloaded, int cameraReserved, int numCenters)
{
	if (numCenters <= 0)
		return false;
	int available = maxPreloaded - cameraReserved;
	if (available <= 0)
		return false;
	return (available / numCenters) >= 9;
}

bool CoverageCharacterInScan(bool nearCamera, int squadRadius, bool retentionPressure)
{
	return nearCamera || (squadRadius >= 1 && !retentionPressure);
}

static volatile LONG s_camGrids     = 0;   // EnqueueCameraGrid calls
static volatile LONG s_camGridZones = 0;   // cells the camera queue took from them

static volatile LONG s_aheadCalls   = 0;   // camera-owned EnqueueAheadZones calls
static volatile LONG s_aheadSteps   = 0;   // ... of which had a direction to extrapolate
static volatile LONG s_aheadZones   = 0;   // cells the camera queue took

static volatile LONG s_scans        = 0;   // character scans that found >= 1 center
static volatile LONG s_scanFull     = 0;   // ... run at 3x3
static volatile LONG s_scanSmall    = 0;   // ... clamped to 2x2 by the budget
static volatile LONG s_scanZones    = 0;   // cells the character queue took

static volatile LONG s_scanDropped  = 0;   // centers past MAX_CHAR_ZONES

static volatile LONG s_tier4        = 0;   // jobs classified "mod-prepared cell"
static volatile LONG s_tier5        = 0;   // jobs classified "everything else"

static void AddNonNegative(volatile LONG* counter, int n)
{
	if (n > 0)
		InterlockedExchangeAdd(counter, (LONG)n);
}

void CoverageNoteCameraGrid(int accepted)
{
	InterlockedIncrement(&s_camGrids);
	AddNonNegative(&s_camGridZones, accepted);
}

void CoverageNoteAheadZones(bool camera, bool stepped, int accepted)
{
	if (!camera)
		return;
	InterlockedIncrement(&s_aheadCalls);
	if (stepped)
		InterlockedIncrement(&s_aheadSteps);
	AddNonNegative(&s_aheadZones, accepted);
}

void CoverageNoteCharScan(int numCenters, bool fullGrid, int accepted)
{
	if (numCenters <= 0)
		return;
	InterlockedIncrement(&s_scans);
	InterlockedIncrement(fullGrid ? &s_scanFull : &s_scanSmall);
	AddNonNegative(&s_scanZones, accepted);
}

void CoverageNoteCentersDropped(int dropped)
{
	AddNonNegative(&s_scanDropped, dropped);
}

void CoverageNoteTier4() { InterlockedIncrement(&s_tier4); }
void CoverageNoteTier5() { InterlockedIncrement(&s_tier5); }

void CoverageResetSession()
{
	InterlockedExchange(&s_camGrids, 0);
	InterlockedExchange(&s_camGridZones, 0);
	InterlockedExchange(&s_aheadCalls, 0);
	InterlockedExchange(&s_aheadSteps, 0);
	InterlockedExchange(&s_aheadZones, 0);
	InterlockedExchange(&s_scans, 0);
	InterlockedExchange(&s_scanFull, 0);
	InterlockedExchange(&s_scanSmall, 0);
	InterlockedExchange(&s_scanZones, 0);
	InterlockedExchange(&s_scanDropped, 0);
	InterlockedExchange(&s_tier4, 0);
	InterlockedExchange(&s_tier5, 0);
}

// Read with InterlockedCompareExchange rather than a plain load so the tier
// counters, which are written off the main thread, are read the same way
// they are written.
static LONG Read(volatile LONG* counter)
{
	return InterlockedCompareExchange(counter, 0, 0);
}

std::string CoverageStatsToken()
{
	std::ostringstream ss;
	ss << " cov=cam3x3:" << Read(&s_camGrids) << "/" << Read(&s_camGridZones)
	   << " ahead:" << Read(&s_aheadCalls) << "/" << Read(&s_aheadSteps)
	   << "/" << Read(&s_aheadZones)
	   << " charGrid:" << Read(&s_scans) << "/" << Read(&s_scanFull)
	   << "/" << Read(&s_scanSmall) << "/" << Read(&s_scanZones)
	   << " drop:" << Read(&s_scanDropped)
	   << " tier:" << Read(&s_tier4) << "/" << Read(&s_tier5);
	return ss.str();
}

bool CoverageDueForPeriodicReport(double nowSec)
{
	// Main thread only, so a plain static is enough. The first call reports
	// immediately: a session that ends inside the first interval should still
	// leave one line behind.
	static double s_lastReport = -1.0;
	if (s_lastReport >= 0.0 && (nowSec - s_lastReport) < 30.0)
		return false;
	s_lastReport = nowSec;
	return true;
}
