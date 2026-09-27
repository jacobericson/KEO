// island_stuck.cpp - Island router emulation and stuck diagnostics.
// Main thread only; reads the builder state without taking locks.

#include "movement/islands.h"
#include "movement/islands_internal.h"
#include "zone/grid.h"

using namespace islands_detail;

// =========================================================================
// Router emulation + PLAYER STUCK support (main thread)
// =========================================================================

bool IslandEmulateCrossing(void* zoneMgr, void* charZone,
                           float destX, float destZ, float posX, float posZ,
                           float* outX, float* outZ)
{
	uintptr_t zm = (uintptr_t)zoneMgr;
	uintptr_t t = (uintptr_t)charZone;
	if (!zm || !t) return false;
	uintptr_t navmesh = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!navmesh) return false;
	float size = *(float*)(KLIB_MEMBER(2, navmesh, NavMesh_cellSize, OFF_NAVMESH_ZONE_SIZE));

	static unsigned short list[ZONE_GRID_COUNT];   // main thread only
	int n = RouterList(zm, t, list, ZONE_GRID_COUNT);

	// Ray from the destination toward the position (pos - dest), t in (0,1).
	float dirX = posX - destX;
	float dirZ = posZ - destZ;
	float best = 1.0f;
	for (int i = 0; i < n; ++i)
	{
		uintptr_t z = ZoneAt(zm, list[i]);
		float minX = *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mCenter_x, OFF_ZONE_AABB_CENTER))     - *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mHalfSize_x, OFF_ZONE_AABB_HALF));
		float minZ = *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mCenter_z, OFF_ZONE_AABB_CENTER + 8)) - *(float*)(KLIB_MEMBER(2, z, ZoneMap_bounds_mHalfSize_z, OFF_ZONE_AABB_HALF + 8));

		float tx;
		if (dirX <= 0.0f) tx = (dirX >= 0.0f) ? 1.0f : ((minX + size) - destX) / dirX;
		else              tx = (minX - destX) / dirX;
		float zc = dirZ * tx + destZ;
		if (minZ > zc || zc > minZ + size) tx = 1.0f;

		float tz;
		if (dirZ <= 0.0f) tz = (dirZ >= 0.0f) ? 1.0f : ((minZ + size) - destZ) / dirZ;
		else              tz = (minZ - destZ) / dirZ;
		float xc = tz * dirX + destX;
		if (minX > xc || xc > minX + size) tz = 1.0f;

		if (tx > 0.0f && best > tx) best = tx;
		if (tz > 0.0f && best > tz) best = tz;
	}
	if (best >= 1.0f) return false;
	*outX = destX + dirX * best;
	*outZ = destZ + dirZ * best;
	return true;
}

bool IslandSampleLabels(void* zoneMgr, float posX, float posZ,
                        float destX, float destZ,
                        int* outSelfLabel, int* outDestLabel, int* outSpan)
{
	if (!zoneMgr || !gridCalibrated || !outSelfLabel || !outDestLabel || !outSpan)
		return false;

	int gx, gy, dgx, dgy;
	if (!WorldToZoneGrid(posX, posZ, &gx, &gy)) return false;
	if (!WorldToZoneGrid(destX, destZ, &dgx, &dgy)) return false;
	uintptr_t charZone = (uintptr_t)GetZoneEntry(zoneMgr, gx, gy);
	uintptr_t destZone = (uintptr_t)GetZoneEntry(zoneMgr, dgx, dgy);
	if (!charZone || !destZone) return false;

	*outSelfLabel = ZoneLabel(charZone);
	*outDestLabel = ZoneLabel(destZone);
	*outSpan = IslandCellSpan(gx, gy, dgx, dgy);
	return true;
}

bool IslandDescribeStuck(void* zoneMgr, uintptr_t charMov,
                         float posX, float posZ, float destX, float destZ,
                         IslandStuckInfo* out)
{
	uintptr_t zm = (uintptr_t)zoneMgr;
	if (!zm || !charMov || !out || !gridCalibrated) return false;

	memset(out, 0, sizeof(*out));
	out->wpX = *(float*)(KLIB_MEMBER(3, charMov, AbstractMovementBase_pathDestination_x, OFF_CMOV_PATH_DEST));
	out->wpZ = *(float*)(KLIB_MEMBER(3, charMov, AbstractMovementBase_pathDestination_z, OFF_CMOV_PATH_DEST + 8));
	out->movingToEdge = *(unsigned char*)(KLIB_MEMBER(3, charMov, CharMovement_movingToEdge, OFF_CMOV_MOVING_TO_EDGE));
	out->edgeCounter = *(int*)(KLIB_MEMBER(3, charMov, CharMovement_edgeTarget, OFF_CMOV_EDGE_COUNTER));
	out->selfComp = -1;
	out->nextComp = -1;
	out->nextGX = out->nextGY = -1;
	out->destGX = out->destGY = -1;
	out->cellSpan = -1;
	out->sameIsland = -1;

	int gx, gy;
	if (!WorldToZoneGrid(posX, posZ, &gx, &gy)) return true;
	uintptr_t charZone = (uintptr_t)GetZoneEntry(zoneMgr, gx, gy);
	if (!charZone) return true;
	out->selfComp = CurComp(zm, charZone);
	out->haveSelf = true;
	out->selfLabel = ZoneLabel(charZone);

	// The destination zone as the engine resolves it: the grid cell holding the
	// order's destination point. Equal labels are the whole of the engine's
	// same-island test, so this pair decides whether an edge route was even
	// considered for this order.
	int dgx, dgy;
	if (WorldToZoneGrid(destX, destZ, &dgx, &dgy))
	{
		uintptr_t destZone = (uintptr_t)GetZoneEntry(zoneMgr, dgx, dgy);
		if (destZone)
		{
			out->haveDest = true;
			out->destLabel = ZoneLabel(destZone);
			out->destGX = dgx; out->destGY = dgy;
			out->cellSpan = IslandCellSpan(gx, gy, dgx, dgy);
			out->sameIsland = (out->selfLabel == out->destLabel) ? 1 : 0;
		}
	}

	float rx, rz;
	int nx = -1, ny = -1;
	if (IslandEmulateCrossing(zoneMgr, (void*)charZone, destX, destZ, posX, posZ, &rx, &rz))
	{
		out->haveCrossing = true;
		float dx = posX - rx, dz = posZ - rz;
		out->xd = sqrtf(dx * dx + dz * dz);
		// The zone just beyond the crossing, toward the destination.
		float tx = destX - rx, tz = destZ - rz;
		float len = sqrtf(tx * tx + tz * tz);
		if (len > 0.001f)
			WorldToZoneGrid(rx + tx / len * 10.0f, rz + tz / len * 10.0f, &nx, &ny);
	}
	else
	{
		// Grid walk from the waypoint toward the destination (dominant axis).
		int wx, wy;
		if (WorldToZoneGrid(out->wpX, out->wpZ, &wx, &wy))
		{
			float dx = destX - out->wpX, dz = destZ - out->wpZ;
			float adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
			nx = wx; ny = wy;
			if (adx >= adz) nx += (dx >= 0) ? 1 : -1;
			else            ny += (dz >= 0) ? 1 : -1;
		}
	}
	if (nx >= 0 && ny >= 0)
	{
		uintptr_t nz = (uintptr_t)GetZoneEntry(zoneMgr, nx, ny);
		if (nz)
		{
			out->nextGX = nx; out->nextGY = ny;
			out->nextComp = CurComp(zm, nz);
			out->nextLabel = ZoneLabel(nz);
			out->nextLoading = ZoneLoadingF(nz) ? 1 : 0;
			out->nextAccess = ZoneAccess(nz) ? 1 : 0;
		}
	}
	return true;
}
