#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_H

// Installs the guard on the node-position helper the cluster-graph search
// calls to turn a packed key into a world position. Present in every build
// and installed unconditionally (prologue permitting): graphPositionGuard
// chooses only whether a firing call is acted on or merely observed, never
// whether the site is watched.
void InstallGraphPositionGuard(int* installed, int*);

#endif // KENSHI_ZONE_OPT_FIXES_GRAPH_POSITION_GUARD_H
