// graph_heuristic_guard.h - The guards on the three unchecked cluster-graph reads inside the
// A* search's hierarchical heuristic. Installed at startup on the main thread while wanted.
#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_HEURISTIC_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_HEURISTIC_GUARD_H

void InstallGraphHeuristicGuard(int* installed, int*);
// "ON" when all three rows installed, "PARTIAL" when some did, "OFF" when none.
const char* GraphHeuristicGuardToken();

#endif
