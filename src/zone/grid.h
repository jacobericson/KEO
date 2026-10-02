// grid.h — Zone grid calibration and world-to-zone coordinate conversion
// Depends on: core.h, game.h

#ifndef KEO_GRID_H
#define KEO_GRID_H

#include "base/core.h"
#include "game/game.h"


// =========================================================================
// Grid calibration state (defined in grid.cpp)
// =========================================================================

extern float zoneOriginX;
extern float zoneOriginZ;
extern float zoneStepX;
extern float zoneStepZ;
extern bool  gridCalibrated;


// =========================================================================
// Grid functions (impl in grid.cpp)
// =========================================================================

void CalibrateZoneGrid(void* zoneMgr);
bool WorldToZoneGrid(float worldX, float worldZ, int* outX, int* outY);


#endif // KEO_GRID_H
