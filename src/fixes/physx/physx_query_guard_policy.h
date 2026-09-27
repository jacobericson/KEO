#ifndef KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_POLICY_H

#include <stddef.h>

// Pure layout, classification and code-generation facts for the guard on the
// result walk inside GameWorld::getObjectsWithinBox. No Windows header, no
// I/O, no game pointers: kept host-testable under tools/tests/.

// --- The site ---------------------------------------------------------
//
// The walk loads each result entry's vptr and calls slot 1 (NxShape::getActor)
// through it. Those two instructions are the six bytes replaced by a jump to
// the guard stub:
//
//   7859EE  48 8B 0C F1   mov  rcx, [rcx+rsi*8]   ; rcx = the NxShape*
//   7859F2  48 8B 01      mov  rax, [rcx]         ; \ the six patched bytes
//   7859F5  FF 50 08      call qword ptr [rax+8]  ; /
//   7859F8  48 8B 48 08   mov  rcx, [rax+8]       ; rax is now getActor()'s
//                                                 ; RETURN VALUE, not the vptr
//   785A40  48 FF C6 ...  inc  rsi; cmp rsi,r13; jl  ; "skip this entry"
//
// So the stub must re-issue the call itself and fall through to kResumeRva
// with rax live. The two return targets are verified as well as the site,
// because the stub branches to both.
extern const unsigned __int64 kPhysQuerySiteRva;
extern const unsigned __int64 kPhysQueryResumeRva;
extern const unsigned __int64 kPhysQuerySkipRva;

// The eight bytes at kPhysQuerySiteRva - 2. The patch is written as one
// aligned qword store (the site's own six bytes sit inside this block, which
// is 8-byte aligned), so the two bytes ahead of the site are part of the
// expected pattern and are preserved verbatim by the new qword.
const int kPhysQuerySiteBlockLen = 8;
extern const unsigned char kPhysQuerySiteBlock[8];
const int kPhysQuerySitePrefixLen = 2;     // bytes of the block ahead of the site

extern const int kPhysQueryResumeLen;      // 4
extern const unsigned char kPhysQueryResumeBytes[4];
extern const int kPhysQuerySkipLen;        // 6
extern const unsigned char kPhysQuerySkipBytes[6];

// Exact compare, always. The build gate's shared-site allowance (an E9 or
// FF 25 head with a matching tail) is correct for a function prologue another
// plugin detoured; nobody legitimately detours an address inside a function
// body, so any difference here means the bytes are not the ones this stub was
// written against, and the guard must not arm.
bool PhysQueryBytesMatch(const unsigned char* actual, const unsigned char* expect, int len);

// --- Classification ---------------------------------------------------
//
// One arm per token on the stats line, because a bare "rejected" count cannot
// separate a use-after-free from an unknown-but-legitimate shape class.
enum PhysQueryClass
{
	PHYSQ_OK = 0,
	PHYSQ_REJ_ADDR,          // NULL, misaligned or non-canonical
	PHYSQ_REJ_UNREAD,        // a guarded read faulted: freed and unmapped
	PHYSQ_REJ_PURE,          // slot 1 is _purecall: freed, block not yet reused
	PHYSQ_REJ_FOREIGN_VPTR,  // vptr outside PhysXCore64
	PHYSQ_REJ_FOREIGN_SLOT   // slot 1 outside PhysXCore64
};

// The two foreign arms are the only ones that can reject a live shape: they
// rest on the assumption that every shape class in the scene has its vtable
// in PhysXCore64. They are counted separately from each other and from the
// rest so the first such rejection in a session can be read as evidence about
// that assumption rather than as an anonymous skip.
bool PhysQueryClassIsForeign(PhysQueryClass cls);

// The cheap arithmetic screen, split out so a test can pin its polarity: a
// shape pointer is 8-byte aligned, non-NULL, and inside the lower canonical
// half of the address space.
bool PhysQueryAddressPlausible(unsigned __int64 p);

// Takes values already read, and whether each read succeeded, so the SEH
// probes stay in the installer's translation unit and every arm -- including
// PHYSQ_REJ_UNREAD -- is reachable from a host test.
//
// purecall == 0 means "the _purecall address could not be confirmed for this
// PhysX build"; the pure test is then skipped and the range tests carry the
// guard alone.
PhysQueryClass ClassifyPhysQueryEntry(unsigned __int64 shape,
                                      bool vptrRead, unsigned __int64 vptr,
                                      bool slot1Read, unsigned __int64 slot1,
                                      unsigned __int64 physBase, unsigned __int64 physEnd,
                                      unsigned __int64 purecall);

// --- Stub code generation ---------------------------------------------

// rel32 displacements reach +-2 GB, which is why the stub cannot live in the
// mod DLL: the game exe and the DLL are tens of gigabytes apart.
bool PhysQueryRel32Reaches(unsigned __int64 from, unsigned __int64 to);

const size_t kPhysQueryStubLen = 0x30;

// Writes the stub at `stubAddr`:
//
//   sub  rsp, 30h                 ; jumped into, so rsp is still 16-aligned
//   mov  [rsp+20h], rcx           ; save the shape pointer
//   call qword ptr [gateSlotAddr] ; __fastcall int(const void*), rcx = shape
//   mov  rcx, [rsp+20h]
//   test eax, eax
//   jz   .skip
//   add  rsp, 30h                 ; rsp back to exactly its vanilla value
//   mov  rax, [rcx]               ; the two original instructions, verbatim
//   call qword ptr [rax+8]
//   jmp  resumeAddr
// .skip:
//   add  rsp, 30h
//   jmp  skipAddr
//
// The gate is called through a slot rather than directly so the slot can be
// pointed at the accept-all thunk if this DLL ever unloads with the patch
// still live. Returns false if the buffer is too small or any displacement
// does not reach.
bool BuildPhysQueryStub(unsigned char* out, size_t cap,
                        unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                        unsigned __int64 resumeAddr, unsigned __int64 skipAddr,
                        size_t* outLen);

// mov eax,1 / ret -- the accept-all replacement for the gate slot.
const size_t kPhysQueryAcceptAllLen = 6;
bool BuildPhysQueryAcceptAll(unsigned char* out, size_t cap);

// The patched form of the eight-byte block: the two prefix bytes unchanged,
// then `E9 rel32` to the stub and one 0x90. Written as a single aligned qword
// store, so a thread already executing the walk sees either the whole old
// instruction pair or the whole jump, never a half-written mixture.
bool BuildPhysQuerySiteQword(const unsigned char* block,
                             unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                             unsigned __int64* outQword);

// --- Deferred-arm retry ------------------------------------------------
//
// PhysXCore64.dll is normally not loaded when the plugin initialises, and the
// guard needs its module range to classify anything, so arming waits for it.
extern const double kPhysQueryRetryIntervalSec;
extern const double kPhysQueryRetryWindowSec;
bool PhysQueryRetryDue(double now, double lastAttempt);
bool PhysQueryRetryExpired(double now, double firstAttempt);

#endif // KENSHI_ZONE_OPT_FIXES_PHYSX_QUERY_GUARD_POLICY_H
