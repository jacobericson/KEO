#ifndef KEO_FIXES_FACTION_RELATIONS_H
#define KEO_FIXES_FACTION_RELATIONS_H

// The faction relations switches: an entry detour on FactionRelations::update
// that finds the faction's own entry with one lookup in place of the engine's
// walk (relationsSelfFind), and pre-call detours on both affectRelations
// overloads that drop a change whose source is the faction itself
// (factionSelfGuard). The detours run on the AI back thread or the main
// thread; the tick runs on the main thread and hands the keys to them. DEV
// builds only: PROD compiles both functions as stubs.

void InstallFactionRelations(int* installed, int*);
void FactionRelationsTick(double now);

#endif // KEO_FIXES_FACTION_RELATIONS_H
