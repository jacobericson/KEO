#include "zone/zone_ledger_core.h"

bool ZoneIdentityEqual(const ZoneIdentity& a, const ZoneIdentity& b)
{
	return a.worldEpoch == b.worldEpoch && a.cellX == b.cellX && a.cellY == b.cellY &&
	       a.contentIncarnation == b.contentIncarnation;
}

bool ZoneIdentitySameCell(const ZoneIdentity& a, const ZoneIdentity& b)
{
	return a.worldEpoch == b.worldEpoch && a.cellX == b.cellX && a.cellY == b.cellY;
}

bool ZoneIdentityIsStaleAttempt(const ZoneIdentity& current, const ZoneIdentity& candidate)
{
	if (!ZoneIdentitySameCell(current, candidate))
		return false;
	return candidate.contentIncarnation < current.contentIncarnation;
}

int ZoneCellIndex(int cellX, int cellY)
{
	if (cellX < 0) cellX = 0;
	if (cellX > ZONE_GRID_DIM - 1) cellX = ZONE_GRID_DIM - 1;
	if (cellY < 0) cellY = 0;
	if (cellY > ZONE_GRID_DIM - 1) cellY = ZONE_GRID_DIM - 1;
	return cellY + (cellX << 6);
}

const char* ZoneLifecycleStateName(int state)
{
	switch (state)
	{
	case ZONE_STATE_UNLOADED:             return "Unloaded";
	case ZONE_STATE_PRIVATE_SHELL:        return "PrivateShell";
	case ZONE_STATE_PRIVATE_CONTENT:      return "PrivateContent";
	case ZONE_STATE_PRIVATE_NAV:          return "PrivateNav";
	case ZONE_STATE_READY_FOR_ADOPTION:   return "ReadyForAdoption";
	case ZONE_STATE_NATIVE_A:             return "NativeA";
	case ZONE_STATE_NATIVE_B_LOADING:     return "NativeBLoading";
	case ZONE_STATE_NATIVE_ACTIVE:        return "NativeActive";
	case ZONE_STATE_RETIRING:             return "Retiring";
	case ZONE_STATE_GEOMETRY_INVALIDATED: return "GeometryInvalidated";
	default:                              return "?";
	}
}

bool ZoneLifecycleCanTransition(int from, int to)
{
	switch (from)
	{
	case ZONE_STATE_UNLOADED:
		return to == ZONE_STATE_PRIVATE_SHELL;

	case ZONE_STATE_PRIVATE_SHELL:
		return to == ZONE_STATE_PRIVATE_CONTENT || to == ZONE_STATE_NATIVE_A ||
		       to == ZONE_STATE_RETIRING;

	case ZONE_STATE_PRIVATE_CONTENT:
		return to == ZONE_STATE_PRIVATE_NAV || to == ZONE_STATE_NATIVE_A ||
		       to == ZONE_STATE_RETIRING;

	case ZONE_STATE_PRIVATE_NAV:
		return to == ZONE_STATE_READY_FOR_ADOPTION || to == ZONE_STATE_NATIVE_A ||
		       to == ZONE_STATE_GEOMETRY_INVALIDATED || to == ZONE_STATE_RETIRING;

	case ZONE_STATE_READY_FOR_ADOPTION:
		return to == ZONE_STATE_NATIVE_A || to == ZONE_STATE_GEOMETRY_INVALIDATED ||
		       to == ZONE_STATE_RETIRING;

	case ZONE_STATE_GEOMETRY_INVALIDATED:
		return to == ZONE_STATE_PRIVATE_CONTENT || to == ZONE_STATE_NATIVE_A ||
		       to == ZONE_STATE_RETIRING;

	case ZONE_STATE_NATIVE_A:
		return to == ZONE_STATE_NATIVE_B_LOADING || to == ZONE_STATE_RETIRING;

	case ZONE_STATE_NATIVE_B_LOADING:
		return to == ZONE_STATE_NATIVE_ACTIVE || to == ZONE_STATE_RETIRING;

	case ZONE_STATE_NATIVE_ACTIVE:
		return to == ZONE_STATE_RETIRING;

	case ZONE_STATE_RETIRING:
		return to == ZONE_STATE_UNLOADED;

	default:
		return false;
	}
}
