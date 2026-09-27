#ifndef KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_H

#include <stddef.h>

// Guards the one-byte store NavMeshGenerator::update makes to `output+0x50`
// for every drained task that is not type 3. On a sector that byte is the
// sector's own `kzReady`; on the 0x48-byte interior NavInstance a stitch task
// can carry, it is past the end of the object, and in the CRT heap's 80-byte
// bucket it lands on byte 0 of the next allocation -- the low byte of that
// object's vptr or first pointer. Nothing reads +0x50 on an interior, so
// skipping the store there changes nothing the game relies on.
//
// A mid-function patch, like the PhysX query guard: eight bytes replaced by a
// jump to a stub near the exe, which asks a gate whether to keep the store.
// The patch installs whatever stitchByteGuard says; the key chooses only
// whether an interior store is skipped (guard) or kept (observe). Both modes
// record the neighbour qword the store hits or would hit, and log it.
//
// The eight bytes cannot be written in one atomic store, so the patch is only
// applied while no path thread exists (pauseState.navmesh is NULL); a later
// call refuses and says why. Call once from startPlugin.
void InstallStitchByteGuard(bool allowed);

// Main thread, once per frame: one log line per recorded interior store, and
// the periodic "StitchByteGuard running:" line.
void StitchByteGuardTick(double now);

// Crash path: the totals and up to maxEntries recent interior stores, most
// recent first, into buf. Fixed buffer, no allocation; every read of game
// memory is fault-guarded. Returns the bytes written.
size_t StitchByteGuardCrashFormat(char* buf, size_t cap, int maxEntries);

// Points the gate slot at a keep-every-store thunk in the stub's own page.
// Call from DLL_PROCESS_DETACH. The patched bytes stay.
void NeutralizeStitchByteGuard();

#endif // KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_H
