// pathfind_diag.cpp - Diagnostic counters and shared probe/player state.
// The path hooks bump the counters with Interlocked* on their own threads.

#include "pathfind/pathfind_diag.h"

namespace pathfind {

PathfindDiagState g_pathDiag;

} // namespace pathfind

namespace pathfind_diag_detail {

union PathfindDiagStatePodCheck { pathfind::PathfindDiagState s; };

} // namespace pathfind_diag_detail

static_assert(__alignof(pathfind::PathfindDiagState) >= 8, "PathfindDiagState must be 8-byte aligned");
