// pathfind_diag_internal.h - Shared main-thread reporter window snapshots.
// Definitions stay with the diagnostic core; no function bodies or initialized state.

#ifndef KENSHI_ZONE_OPT_PATHFIND_DIAG_INTERNAL_H
#define KENSHI_ZONE_OPT_PATHFIND_DIAG_INTERNAL_H

namespace pathfind_diag_detail {

extern double lastPathDiagLogTime;
extern long prevPrimaryAttempts;
extern long prevPrimarySuccess;
extern long prevPrimaryFail;
extern long prevAstarAttempts;
extern long prevAstarSuccess;
extern long prevAstarUnreach;
extern long prevAstarTerminated;
extern long prevAstarInvalid;
extern long prevConnAttempts;
extern long prevConnFail;
extern long prevPlayerRequests;
extern long prevNPCRequests;
extern long prevPlayerCap;
extern long prevFailSequence;

} // namespace pathfind_diag_detail
using namespace pathfind_diag_detail;

#endif // KENSHI_ZONE_OPT_PATHFIND_DIAG_INTERNAL_H
