#ifndef KEO_FIXES_CORPSE_PIN_H
#define KEO_FIXES_CORPSE_PIN_H

// Detour on ActivePlatoon::calculateCurrentPos: a squad whose only living
// content is a carried NPC corpse otherwise reports the pickup spot forever
// (vanilla averages live members only), which is what pins the squad's
// unload check, save position and "wandered out of zone" test to a cell the
// carrier long since left. This repositions
// such a squad to the carrier instead, on top of whatever the original
// already computed.
//
// Main thread only: the sole caller, ActivePlatoon::update, runs there.
void InstallCorpsePin(int* installed, int*);

// Diagnostic counters (Interlocked reads); both 0 if the fix is off or has
// never applied.
long CorpsePinOverrideCount();
long CorpsePinNoCarrierCount();

// Call once per frame from the main thread. Unconditional: prints a
// "CorpsePin:" line on a timer whether or not the fix is enabled or ever
// applied, so a session log can tell "never fired" from "never printed".
void CorpsePinTick(double now);

#endif // KEO_FIXES_CORPSE_PIN_H
