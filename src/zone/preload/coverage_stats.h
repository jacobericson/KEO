// Preload coverage counters, and the one budget predicate behind the
// character scan's grid choice.
//
// The counters are session-cumulative and every one of them is printed
// whether or not it moved, so a zero reads as "this path was never reached",
// not as "nothing happened during this transition". They are also printed
// unconditionally rather than through a debug-only log, because the paths
// they describe are on in a stripped build too.
#ifndef KEO_COVERAGE_STATS_H
#define KEO_COVERAGE_STATS_H

#include <string>

// True when the character preload budget can give every scan center a full
// 3x3 (9 cells) rather than a 2x2. With no centers there is nothing to
// cover, so the answer is false and the caller enqueues nothing either way.
bool CoverageFullGridAllowed(int maxPreloaded, int cameraReserved, int numCenters);

// Whether the character scan preloads around a character: always within the
// camera's radius, and away from it only while the squad radius keeps a ring
// (squadRadius >= 1) and the retention hold is not past its cap.
bool CoverageCharacterInScan(bool nearCamera, int squadRadius, bool retentionPressure);

// Whether the scan queues one cell of a scanned character's grid: any cell
// while the squads keep a ring, otherwise only one near the camera at the
// radius the hold keeps right now (the caller narrows it under pressure), as
// nothing else would hold it once it loads.
bool CoverageCellInScan(bool cellNearCamera, int squadRadius);

// accepted = cells the queue actually took; the rest were duplicates or
// refusals, which is the difference between "restored" and "covering".
void CoverageNoteCameraGrid(int accepted);

// stepped = the call had a real direction to extrapolate along. A call with
// no step accepts nothing and must not read as a queue refusal.
void CoverageNoteAheadZones(bool camera, bool stepped, int accepted);

void CoverageNoteCharScan(int numCenters, bool fullGrid, int accepted);

// Scan cells past MAX_CHAR_ZONES, which get no grid at all. Reported apart
// from the scan count, which is per scan and cannot express it.
void CoverageNoteCentersDropped(int dropped);

// Called from the navmesh queue-prioritization pass, off the main thread.
void CoverageNoteTier4();
void CoverageNoteTier5();

void CoverageResetSession();

std::string CoverageStatsToken();

// Main thread, once per frame: true at most once per 30 s, so a session with
// no transition at all still reports. Logging stays with the caller, which
// keeps this unit free of the log and game headers; the answer does not
// depend on anything the counters hold, so it is true whether or not any of
// them moved.
bool CoverageDueForPeriodicReport(double nowSec);

#endif
