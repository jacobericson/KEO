// astar_hier.h - The optional hierarchical arm of the A* search: the sequence around the
// original call, its counters and its log line.
// AstarHierSearch runs on whichever thread calls findPathFull (the path thread for a character's
// search) and takes no lock and allocates nothing; AstarHierTick is main-thread only.
#ifndef KENSHI_ZONE_OPT_ASTAR_HIER_H
#define KENSHI_ZONE_OPT_ASTAR_HIER_H

// Replaces the one original findPathFull call. playerByReq is the queue-priority label
// (-1 unknown), used only as the cross-check labelDisagree counts.
void AstarHierSearch(void* streamingCollection, void* findPathInput, void* findPathOutput,
                     void* returnAddr, int playerByReq);

// The AstarHier: line and any pending AstarHier event: lines, on the path pool's window;
// silent while playerHierarchical is off.
void AstarHierTick();

#endif
