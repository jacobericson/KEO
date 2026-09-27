#ifndef KENSHI_ZONE_OPT_FIXES_NEST_VALIDATION_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_NEST_VALIDATION_POLICY_H

// Decision logic behind the nest-validation guard on
// SectionManager::finalizeZoneResources (nest_validation.h). Pure arithmetic,
// host-testable and independent of the ledger's storage and of any game or
// KenshiLib header.

// The AreaSector record's own grid coordinates. Verified in the live IDB:
// finalizeZoneResources reads its argument's first two 32-bit fields
// directly as the (x, y) it passes to ZoneManager::getZoneMap_int to resolve
// the nests' owning cell, and SectionManager::registerZoneCenter computes
// the same record from the same (x, y) as base + 136 + 184*(y + (x<<6)).
// Coordinates are 0..63 on each axis; anything else is a record this guard
// does not recognise. outCellIndex uses the same row-major order.
bool NestValidationCellFromRecord(unsigned int rawX, unsigned int rawY, int* outCellIndex);

enum NestValidationAction
{
	NESTVAL_SKIP,    // the mesh is not in for this cell: return without calling the original
	NESTVAL_PROCEED  // the mesh is in: call the original as normal
};

struct NestValidationDecision
{
	NestValidationAction action;
	bool countSkip;         // this call starts a new skip streak on this cell
	bool countRevalidated;  // this call proceeds after a skip streak on the same cell
};

// meshReady: the original readiness function's answer for this cell.
// wasPreviouslySkipped: the per-cell ledger's flag, set by an earlier skip
// and expected to be cleared by the caller once counted here.
//
// A cell not ready across several consecutive calls is one streak, not one
// skip per call: countSkip fires only on the streak's first call
// (!wasPreviouslySkipped), and countRevalidated fires once on the proceed
// that ends it. Counting every call in the streak would make a healthy,
// still-loading cell look identical to a cell whose mesh never arrives --
// both show skipped > revalidated -- so a raw per-call count cannot support
// the "revalidated == skipped once loading settles" invariant this exists to
// check. Per-streak counting can.
NestValidationDecision NestValidationDecide(bool meshReady, bool wasPreviouslySkipped);

#endif // KENSHI_ZONE_OPT_FIXES_NEST_VALIDATION_POLICY_H
