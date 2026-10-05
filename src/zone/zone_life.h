// zone_life.h - Public retention queries for zone lifecycle.
// Lifecycle APIs are in preload.h/preload_internal.h; private records and helpers are in zone_life_internal.h.
// Implementations cover registration, unload, leak reporting, first-time handoff and eviction.
// All retention queries below run on the main thread.
#ifndef KEO_ZONE_LIFE_H
#define KEO_ZONE_LIFE_H

#include "zone/preload/preload.h"

// The retention set, shared with whatever else needs it. Two levels, and
// they answer different questions:
//
//   ZlRetentionReadable  one fact about the last rebuild: every anchor was
//                        readable. False means the map's zeroes mean
//                        "could not tell", not "far", and no caller may act
//                        on them.
//   ZlRetentionNear      the per-cell bit that rebuild left, up to a second
//                        old: this cell was within its anchor's configured
//                        radius of some anchor (the camera's radius around
//                        the camera, the squads' around player characters).
//
// ZlAnchorsNearCell is the live single-cell question, read now rather than
// from the map, at radii the caller chooses: rCamera around the camera's
// cell and the central zone, rSquad around each player character. Its
// fail-open rule is its own and is NOT the rebuild's: it answers "near" when
// the zone manager, the grid or the player list cannot be read, but a
// missing camera anchor only makes it fall through to the players, so with
// those readable and far away it answers "not near" on a world with no
// camera.
//
// ZlCellNearCamera: within r of the camera's cell or the central zone, and
// "near" when neither can be read.
void ZlRetentionRefresh(void* zoneMgr);
bool ZlRetentionReadable();
bool ZlRetentionNear(int cell);
bool ZlAnchorsNearCell(void* zoneMgr, int gx, int gy, int rCamera, int rSquad);
bool ZlCellNearCamera(void* zoneMgr, int gx, int gy, int r);

#endif // KEO_ZONE_LIFE_H
