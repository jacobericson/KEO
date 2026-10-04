// formation_pace.h - The gather pacing: while a run-together group gathers, each gathering member's
// speed is scaled by its share of the farthest member's distance to the gather point, through a
// post-call detour on SpeedGroup::getSpeed that reads the factor table the formation poll publishes.
// Each member's arrival, distance and paced mark live here, keyed by group slot and member index.
// Main thread, except the detour (the AI back thread, or the main thread with
// characterMultithreading off).
#ifndef KEO_FORMATION_PACE_H
#define KEO_FORMATION_PACE_H

#include <sstream>
#include "movement/formation.h"

// startPlugin, once: installs the detour while formationGatherPace is on, and logs pace=.
void InstallFormationPace(int* installed, int* total);
// CreateFormationGroup, once its slot g is chosen: forgets every member slot of group g (no arrival,
// no distance, not paced), whatever the key says.
void FormationPaceResetGroup(int g);
// PollFormationGather's loop, for each member slot m of group g it visits: m is not paced this poll
// unless FormationPaceNote runs for it below. Whatever the key says.
void FormationPaceForget(int g, int m);
// PollFormationGather's proximity test: member m of group g stands distSq (squared, x-z) from the
// gather point; its first poll within gatherRadiusSq stamps its arrival at now. Whatever the key
// says.
void FormationPaceNote(int g, int m, float distSq, float gatherRadiusSq, double now);
// PollFormationGroups, before its group loop: empties the staged set. Returns at once unless armed.
void FormationPaceFrameBegin();
// Group g still gathering after its gather loop: stages each factor below 1 of the members the loop
// noted this poll and marks them paced. Returns at once unless armed.
void FormationPaceStageGroup(int g, const FormationGroup& grp);
// PollFormationGroups, after its group loop: publishes the staged set. Returns at once unless armed.
void FormationPaceFramePublish();
// ClearFormationGroups (a save load, the install): forgets every group's member slots, then, when
// armed, publishes an empty table.
void FormationPaceClear();
// The Formation gathered: line's " spread=<s> paced=<n>" for group g, whatever the key says.
void FormationPaceAppendGathered(int g, const FormationGroup& grp, std::ostringstream& ss);
// The Islands: line's " paceMin=<f|-|off> paceHits=<n>": the smallest factor staged and the detour's
// paced answers since the previous call, whose window it restarts. Main thread.
void FormationPaceAppendDiag(std::ostringstream& ss);

#endif // KEO_FORMATION_PACE_H
