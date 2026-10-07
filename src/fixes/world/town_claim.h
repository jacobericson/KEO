#ifndef KEO_FIXES_TOWN_CLAIM_H
#define KEO_FIXES_TOWN_CLAIM_H

// A building the player places keeps a player town. Two detours: the builder holds a
// thread-local placement depth around its call; createBuilding, inside that depth and for an
// unparented player placement only, replaces a null town (or a non-player town inherited from
// a snap target the player does not own) with the owner's town covering the spot, else noTown,
// and raises a thread-local flag for exactly that call. A mid-function patch in createBuilding
// asks a gate that reads and clears the flag, and skips the first-time zone's re-check of the
// town. Main thread.

namespace fixes {

// A kInstallSteps entry: both rows while townClaimFix is on.
void InstallTownClaim(int* installed, int* total);
bool TownClaimRowsInstalled();
// From InstallHooksAndGuards, before any world exists: verifies the site's four byte groups and
// writes the patch. Returns at its first statement while townClaimFix is off; writes nothing
// when the build gate refused or the rows are not installed.
void InstallTownClaimPatch(bool gateOk);
// DLL_PROCESS_DETACH: points the gate slot at the vanilla thunk. The patched bytes stay.
void NeutralizeTownClaimPatch();
// Main thread: the patch's state, 1 armed, -1 refused, 0 not attempted.
int TownClaimPatchState();
// Main thread, from the camera tick's list. Returns at once while townClaimFix is off. DEV: once
// a minute, when a counter moved since the last one, the "TownClaim: heartbeat" line. PROD: nothing.
void TownClaimTick(double now);

} // namespace fixes

// The stub's gate, called by address from the patched site: the placement's flag, cleared.
extern "C" int __fastcall KEO_TownClaimGate(void);

#endif // KEO_FIXES_TOWN_CLAIM_H
