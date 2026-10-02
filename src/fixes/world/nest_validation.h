#ifndef KEO_FIXES_NEST_VALIDATION_H
#define KEO_FIXES_NEST_VALIDATION_H

// Detour on SectionManager::finalizeZoneResources: the native function
// destroys (destroyPhysical + TownList::destroy, permanent) any nest whose
// position fails NavMesh::getPositionValid, with no check that the cell's
// mesh is in at all. A record reached before its mesh lands -- possible any
// time the mod's own generation or preload lags native promotion -- loses
// nests that were never invalid.
//
// The detour resolves the record back to its cell, asks the original
// readiness function (not the mod's isContentPending bypass) and returns
// without calling the original when the mesh is not in. Nothing is lost:
// native phase 5 re-runs this validation on every Set B zone every loading
// cycle, so a skipped cell is revalidated on the first cycle after its mesh
// lands.
//
// Main thread only: the sole caller, ZoneManager::processLoading, runs there.
void InstallNestValidationGuard(int* installed, int*);

// Diagnostic counters (Interlocked reads); all 0 if the guard is off or has
// never applied.
//
// skipped/revalidated count skip *streaks*, not calls: a cell not ready
// across several consecutive loading cycles is one streak, counted once when
// it starts and once when it ends. In a healthy session, once loading is
// idle (no active transition, nothing left in the queue), revalidated ==
// skipped -- every streak that started also ended. A standing, unmoving gap
// while loading is idle names a cell whose mesh never arrived; a gap while
// loading is still active is an in-progress streak, not a fault.
//
// destroyed counts an actual TownList::destroy call made from inside the
// guard's own call to the original finalizeZoneResources -- i.e. a real,
// permanent nest destroy that the guard's readiness check let through. The
// pairing invariant above proves only that a skip was eventually retried, not
// that what it let through afterwards was a genuinely invalid nest, so this
// is the only one of the three counters that can support a destroyed=0
// claim.
long NestValidationSkippedCount();
long NestValidationRevalidatedCount();
long NestValidationDestroyedCount();

// Clears the per-cell skip ledger. Call once from the save-load reset path
// (world identity changes there): otherwise a cell flagged skipped in a
// previous world could make the first legitimate proceed on the same cell
// index, in the new world, count as a spurious revalidation. Never affects
// the skip/proceed decision itself, which depends only on the readiness
// answer -- this only reduces cross-world noise in the counters, and does not
// make revalidated == skipped an exact invariant: a streak already counted as
// a skip at the moment of reset is cleared here without a matching
// revalidation, so it stays permanently unpaired in that session's tally.
// Main thread only, like everything else here.
void NestValidationClearOnReset();

#endif // KEO_FIXES_NEST_VALIDATION_H
