#ifndef KEO_FIXES_FACTION_RELATIONS_H
#define KEO_FIXES_FACTION_RELATIONS_H

// The faction self-relation lookup replaces the engine's map walk with a
// verified bucket lookup. The detour runs on the AI back thread or the main
// thread; the main-thread tick publishes the key. DEV builds also record
// counters and write the Relations: heartbeat.

void InstallFactionRelations(int* installed, int*);
void FactionRelationsTick(double now);

#endif // KEO_FIXES_FACTION_RELATIONS_H
