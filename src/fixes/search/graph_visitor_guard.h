#ifndef KEO_FIXES_GRAPH_VISITOR_GUARD_H
#define KEO_FIXES_GRAPH_VISITOR_GUARD_H

// The absent-instance test the A* heuristic never makes. Present in every
// build: the fault it prevents is a measured mid-session crash on the search
// thread.
void InstallGraphVisitorGuard(int* installed, int*);

#endif
