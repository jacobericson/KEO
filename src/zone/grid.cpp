// Zone grid calibration and world-to-zone coordinate conversion.
// Extracted from preload.cpp — pure coordinate math, no preload logic.

#include "zone/grid.h"


// =========================================================================
// Grid calibration state
// =========================================================================

float zoneOriginX    = 0.0f;
float zoneOriginZ    = 0.0f;
float zoneStepX      = 0.0f;
float zoneStepZ      = 0.0f;
bool  gridCalibrated = false;


// =========================================================================
// Grid calibration
// =========================================================================

void CalibrateZoneGrid(void* zoneMgr)
{
	if (gridCalibrated)
		return;

	void* zone00 = GetZoneEntry(zoneMgr, 0, 0);
	void* zone10 = GetZoneEntry(zoneMgr, 1, 0);
	void* zone01 = GetZoneEntry(zoneMgr, 0, 1);
	if (!zone00 || !zone10 || !zone01)
		return;

	float x00 = GetZoneCenterX(zone00);
	float z00 = GetZoneCenterZ(zone00);
	float x10 = GetZoneCenterX(zone10);
	float z01 = GetZoneCenterZ(zone01);

	float stepX = x10 - x00;
	float stepZ = z01 - z00;

	if (stepX <= 0.0f || stepZ <= 0.0f || stepX > 50000.0f || stepZ > 50000.0f)
	{
		LogMsg("Grid calibration FAILED: invalid step size");
		return;
	}

	zoneOriginX = x00;
	zoneOriginZ = z00;
	zoneStepX = stepX;
	zoneStepZ = stepZ;
	gridCalibrated = true;

	std::ostringstream ss;
	ss << "Grid calibrated: origin=(" << std::fixed << std::setprecision(1)
	   << zoneOriginX << "," << zoneOriginZ << ") step=("
	   << zoneStepX << "," << zoneStepZ << ")";
	LogMsg(ss.str());
}

bool WorldToZoneGrid(float worldX, float worldZ, int* outX, int* outY)
{
	if (!gridCalibrated)
		return false;

	float relX = (worldX - zoneOriginX) / zoneStepX;
	float relZ = (worldZ - zoneOriginZ) / zoneStepZ;

	int gx = (int)floorf(relX + 0.5f);
	int gz = (int)floorf(relZ + 0.5f);
	if (gx < 0) gx = 0;
	if (gx > 63) gx = 63;
	if (gz < 0) gz = 0;
	if (gz > 63) gz = 63;

	*outX = gx;
	*outY = gz;
	return true;
}
