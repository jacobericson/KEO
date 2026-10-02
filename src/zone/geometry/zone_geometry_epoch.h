#ifndef KEO_ZONE_GEOMETRY_EPOCH_H
#define KEO_ZONE_GEOMETRY_EPOCH_H

// The conservative global geometry epoch: one atomic word carrying how many
// geometry mutations have completed and how many are in flight, plus the two
// call sites that read it (a generation's capture, and the L1 store point's
// check). The rules it feeds are in zone_geometry_cert.h.
//
// Locking: none. The word is read and written with interlocked operations
// only, so no lock of this module's own can appear in any order, and the
// capture site -- which runs with processJobCS held, on a thread that is
// about to release it -- composes with nothing. Nothing here waits, sleeps
// or retries.
//
// Coverage: the epoch is bumped at the geometry boundaries the mod itself
// drives. The engine's own building publication and removal are not among
// them, so a current certificate means "the mod moved no geometry", not
// "no geometry moved". That gap is why the late-adoption mode is fenced.

#include "base/config.h"
#include "zone/geometry/zone_geometry_cert.h"

#if ZONEHAND_STEP >= 1

// One atomic read of the counter. Any thread.
ZoneGeometrySnapshot ZoneGeometrySnapshotNow();

// A geometry mutation this mod drives. Raises the in-flight depth on entry;
// lowers it and raises the epoch, in one atomic step, on exit. Main thread
// in every current use, but correct from any.
void ZoneGeometryMutationBegin();
void ZoneGeometryMutationEnd();

// RAII so an unwinding call cannot leave the depth raised for the rest of
// the session, which would refuse every certificate from then on.
struct ZoneGeometryMutationScope
{
	ZoneGeometryMutationScope()  { ZoneGeometryMutationBegin(); }
	~ZoneGeometryMutationScope() { ZoneGeometryMutationEnd(); }
};

// A boundary with no duration to bracket (a world reset): raises the epoch
// alone.
void ZoneGeometryNoteBoundary();

// The mode in force. A runtime read, so the production store point below
// tests a value rather than a constant; it answers ZONE_GEOMETRY_CONTENT_ONLY
// in every build that can be produced today.
ZoneGeometryMode ZoneGeometryActiveMode();

// Generation side, called on the NavMesh background thread or a worker just
// before the run that reads geometry. Stores the certificate in this
// thread's slot, replacing whatever the thread's previous job left there.
void ZoneGeometryCaptureForJob(int gridX, int gridY);

// Store side, called at the L1 store point with processJobCS held. Takes the
// second snapshot, checks this thread's certificate against it, counts the
// verdict, and answers whether publication is refused. The verdict is
// counted whether or not it is acted on: the counts are the measurement that
// has to come back from a session before the mode could be anything else.
bool ZoneGeometryStoreRefused(int gridX, int gridY);

// Periodic report, main thread.
void ZoneGeometryCertTick(double now);

// World reset: clears the counts. The epoch itself is monotonic across a
// reset (the boundary bump is a real geometry removal).
void ZoneGeometryCertReset();

#else

inline void ZoneGeometryMutationBegin() {}
inline void ZoneGeometryMutationEnd() {}
struct ZoneGeometryMutationScope { };
inline void ZoneGeometryNoteBoundary() {}
inline void ZoneGeometryCaptureForJob(int, int) {}
inline bool ZoneGeometryStoreRefused(int, int) { return false; }
inline void ZoneGeometryCertTick(double) {}
inline void ZoneGeometryCertReset() {}

#endif // ZONEHAND_STEP >= 1

#endif // KEO_ZONE_GEOMETRY_EPOCH_H
