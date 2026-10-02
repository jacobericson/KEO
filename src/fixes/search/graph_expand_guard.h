#ifndef KEO_FIXES_GRAPH_EXPAND_GUARD_H
#define KEO_FIXES_GRAPH_EXPAND_GUARD_H

// Installs the guard on the A* iteration that pops and expands the next
// search node. Present in every build; gated on graphExpandGuard.
void InstallGraphExpandGuard(int* installed, int*);

#endif // KEO_FIXES_GRAPH_EXPAND_GUARD_H
