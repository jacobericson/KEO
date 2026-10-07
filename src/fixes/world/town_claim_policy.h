#ifndef KEO_FIXES_TOWN_CLAIM_POLICY_H
#define KEO_FIXES_TOWN_CLAIM_POLICY_H

#include <stddef.h>

// Pure decisions and code generation for the player-placement town fix in
// RootObjectFactory::createBuilding. No Windows header, no game pointers: host-tested.

// --- The decision -----------------------------------------------------

enum TownClaimAction { TC_PASS = 0, TC_USE_CONTAINING, TC_USE_NULL_TOWN, TC_VANILLA };

struct TownClaimInputs
{
	bool depthSet;                  // inside the builder's placement call
	bool ownerIsPlayer;             // owner->isPlayer is set
	bool isFoliage;
	bool hasFurnitureOf;
	bool hasDoorOf;
	bool hasIndoorsOf;
	bool hasSaveState;
	bool townIsNull;
	bool townIsPlayerTown;          // t's faction is a player faction
	bool hasSnapTarget;             // a mounted-building callback with a non-null target
	bool snapTargetIsPlayerOwned;   // that target's faction is a player faction
	bool haveContainingPlayerTown;  // one of the owner's towns covers the spot
	bool spotInNpcTown;             // an NPC town's radius holds the spot (player towns skipped)
	bool townHoldsSpot;             // t is non-null and its own radius holds the spot
	bool createsPlayerTown;         // the building's data has "creates player town" set
};

// A player placement with no parent and no saved state: the call whose town the fix keeps and
// whose first-time re-check the patched site skips.
bool TownClaimFlagged(const TownClaimInputs& in);
// Flagged, and the town vanilla chose cannot stand: none at all, or a non-player town inherited
// from a snap target the player does not own.
bool TownClaimNeedsTown(const TownClaimInputs& in);
// TC_VANILLA when TownClaimVanillaStands. PASS keeps t. Otherwise the containing player town
// when there is one, else noTown: a flagged call is never handed on with a null town by the fix.
TownClaimAction TownClaimDecide(const TownClaimInputs& in);
// The one case where the game's own choice gives an NPC town and this fix would hand a player
// town instead: the game's choice stands (its town, its first-time re-check). Open ground inside
// an NPC town's radius, an owner's town over the spot, and either no town or a player town whose
// own radius misses the spot; never a building that creates a player town, which the NPC town
// would make undismantlable.
bool TownClaimVanillaStands(const TownClaimInputs& in);
// Whether the call goes on with the skip flag set: a flagged call with a town, unless the game's
// choice stands.
bool TownClaimSetsSkipFlag(TownClaimAction action, bool flagged, bool haveTown);

// One of the owner's towns, centre and radius in world units.
struct TownClaimCandidate { float x, z, radius; };
const int kTownClaimMaxCandidates = 64;
// The candidate whose radius covers (px, pz) by horizontal distance, inclusive, nearest centre
// first; -1 when none does. A radius of 0 or less covers nothing.
int TownClaimPickContaining(const TownClaimCandidate* c, int n, float px, float pz);
// A town the containing-town walk may pick: the owner's faction, not a nest marker and not a
// nest, the same exclusions as the game's own TownList::getNearestWithinItsRadius.
bool TownClaimTownEligible(bool factionMatches, bool nestMarker, bool isNest);

// --- The snap callback's vftable --------------------------------------
//
// The builder (0x4D6810) stores SetMountedBuildingCallback's vftable with
//   4D6D33  48 8D 15 96 7C 20 01    lea  rdx, [rip+1207C96h]     ; 0x16DE9D0
// so a binary whose lea no longer reaches the vftable the snap test compares is refused.
const size_t kTownClaimSnapLeaOffset = 0x523;   // from the builder's entry
const int    kTownClaimSnapLeaLen    = 7;
extern const unsigned char kTownClaimSnapLeaBytes[7];
// True when `bytes` (at address `at`) is `lea rdx, [rip+disp32]` and its target is `expect`.
bool TownClaimLeaReaches(const unsigned char* bytes, unsigned __int64 at, unsigned __int64 expect);

// --- The site ---------------------------------------------------------
//
//   57C9AC  48 8B 7C 24 78          mov  rdi, [rsp+78h]          ; ZoneMapContent*
//   57C9B1  80 BF A8 00 00 00 00    cmp  byte ptr [rdi+0A8h], 0  ; \ the thirteen
//   57C9B8  0F 84 C0 02 00 00       jz   57CC7E                  ; / patched bytes
//   57C9BE  80 BD 78 04 00 00 00    cmp  byte ptr [rbp+478h], 0  ; resume: first-time path
//   57CC7E  48 85 DB 0F 85 48 02 00 00 EB 05                     ; keep: test rbx / jnz / jmp
//
// rbx holds the town; nothing branches into the thirteen bytes.
extern const unsigned __int64 kTownClaimLeadRva;     // 0x57C9AC
extern const unsigned __int64 kTownClaimSiteRva;     // 0x57C9B1
extern const unsigned __int64 kTownClaimResumeRva;   // 0x57C9BE
extern const unsigned __int64 kTownClaimKeepRva;     // 0x57CC7E
const int kTownClaimLeadLen   = 5;
const int kTownClaimSiteLen   = 13;
const int kTownClaimResumeLen = 7;
const int kTownClaimKeepLen   = 11;
extern const unsigned char kTownClaimLeadBytes[5];
extern const unsigned char kTownClaimSiteBytes[13];
extern const unsigned char kTownClaimResumeBytes[7];
extern const unsigned char kTownClaimKeepBytes[11];

// Exact compare: mid-function, a difference means an unknown binary.
bool TownClaimBytesMatch(const unsigned char* actual, const unsigned char* expect, int len);
bool TownClaimRel32Reaches(unsigned __int64 from, unsigned __int64 to);

const size_t kTownClaimStubLen = 0x3E;

// Writes the stub at `stubAddr`:
//
//   cmp  byte ptr [rdi+0A8h], 0           ; the replayed test
//   je   keepAddr                         ; a saved zone: vanilla, the gate is not asked
//   push rax, rcx, rdx, r8, r9, r10, r11  ; 7 pushes + 28h keep rsp 16-aligned
//   sub  rsp, 28h
//   call qword ptr [gateSlotAddr]         ; int __fastcall gate(void)
//   add  rsp, 28h
//   test eax, eax
//   pop  r11, r10, r9, r8, rdx, rcx, rax  ; pops leave the flags alone
//   jnz  keepAddr                         ; a flagged placement keeps its town
//   jmp  resumeAddr                       ; the first-time path, as vanilla
//
// No XMM register is saved: every path out of the site (resume, keep) reaches a call before it
// reads one, so none is live across the gate.
bool BuildTownClaimStub(unsigned char* out, size_t cap,
                        unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                        unsigned __int64 keepAddr, unsigned __int64 resumeAddr, size_t* outLen);

// xor eax,eax / ret -- the vanilla replacement for the gate slot.
const size_t kTownClaimVanillaGateLen = 3;
bool BuildTownClaimVanillaGate(unsigned char* out, size_t cap);

// The patched site: `E9 rel32` to the stub and eight 0x90. Refuses unless `current` is the
// thirteen original bytes and the stub is in rel32 reach.
bool BuildTownClaimSitePatch(const unsigned char* current,
                             unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                             unsigned char* out13);

#endif // KEO_FIXES_TOWN_CLAIM_POLICY_H
