#ifndef KEO_FIXES_TOWN_CLAIM_POLICY_H
#define KEO_FIXES_TOWN_CLAIM_POLICY_H

#include <stddef.h>

// Pure decisions and code generation for the player-placement town fix in
// RootObjectFactory::createBuilding. No Windows header, no game pointers: host-tested.

// --- The decision -----------------------------------------------------

enum TownClaimAction { TC_PASS = 0, TC_USE_CONTAINING, TC_USE_NULL_TOWN };

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
};

// A player placement with no parent and no saved state: the call whose town the fix keeps and
// whose first-time re-check the patched site skips.
bool TownClaimFlagged(const TownClaimInputs& in);
// Flagged, and the town vanilla chose cannot stand: none at all, or a non-player town inherited
// from a snap target the player does not own.
bool TownClaimNeedsTown(const TownClaimInputs& in);
// PASS keeps t. Otherwise the containing player town when there is one, else noTown: a flagged
// call is never handed on with a null town.
TownClaimAction TownClaimDecide(const TownClaimInputs& in);

// One of the owner's towns, centre and radius in world units.
struct TownClaimCandidate { float x, z, radius; };
const int kTownClaimMaxCandidates = 64;
// The candidate whose radius covers (px, pz) by horizontal distance, inclusive, nearest centre
// first; -1 when none does. A radius of 0 or less covers nothing.
int TownClaimPickContaining(const TownClaimCandidate* c, int n, float px, float pz);

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
