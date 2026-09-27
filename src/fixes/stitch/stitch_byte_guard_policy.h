#ifndef KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_POLICY_H

#include <stddef.h>

// Pure layout, classification and code-generation facts for the guard on the
// `kzReady` store in NavMeshGenerator::update. No Windows header, no I/O, no
// game pointers: kept host-testable under tools/tests/.

// --- The site ---------------------------------------------------------
//
// Every drained task whose type is not 3 reaches:
//
//   3C8D67  41 83 FF 03         cmp  r15d, 3          ; r15d = task type
//   3C8D6B  0F 85 AD 03 00 00   jnz  3C911E           ; the only way in
//   ...
//   3C911E  C6 46 50 00         mov  byte [rsi+50h],0 ; \ the eight patched
//   3C9122  48 8B 46 08         mov  rax, [rsi+8]     ; / bytes
//   3C9126  44 89 44 24 20      mov  [rsp+20h], r8d   ; resume; r8d is live
//
// rsi is the task's output. For a type-4 (stitch) task that is the
// NavInstance addStitchJob was given: a 0x88-byte NavMeshSector, for which
// +0x50 is its own `kzReady` byte, or a 0x48-byte interior NavInstance, for
// which +0x50 lies past the end of the object -- byte 0 of the next block in
// the CRT heap's 80-byte bucket. Nothing reads +0x50 on an interior.
extern const unsigned __int64 kStitchByteSiteRva;
extern const unsigned __int64 kStitchByteResumeRva;
extern const unsigned __int64 kStitchByteBranchRva;

const int kStitchByteSiteLen = 8;
extern const unsigned char kStitchByteSiteBytes[8];
const int kStitchByteResumeLen = 5;
extern const unsigned char kStitchByteResumeBytes[5];
// The compare and branch that lead in: they pin r15d as the type and the
// site as the branch's target in this build.
const int kStitchByteBranchLen = 10;
extern const unsigned char kStitchByteBranchBytes[10];

// The NavInstance fields the classifier reads.
const size_t kStitchByteUidOffset   = 0x3C;
const size_t kStitchByteWriteOffset = 0x50;

// addStitchJob's own test (`cmp dword [job+3Ch], 0FFFFh / jnb`): below it is
// a sector, at or above it an interior.
const unsigned int kStitchByteSectorUidLimit = 0xFFFF;
const int kStitchByteStitchType = 4;

// Exact compare, always: the site is mid-function, so a difference means an
// unknown binary, never a site another plugin shares.
bool StitchByteBytesMatch(const unsigned char* actual, const unsigned char* expect, int len);

// --- Classification ---------------------------------------------------

enum StitchByteClass
{
	SBG_IN_OBJECT = 0,       // not a stitch task: output is the task's own sector
	SBG_STITCH_SECTOR,       // stitch of a 0x88 sector: +0x50 is kzReady
	SBG_STITCH_INTERIOR,     // stitch of a 0x48 interior: +0x50 is the next block
	SBG_STITCH_UNKNOWN       // stitch whose uid could not be read
};

StitchByteClass ClassifyStitchByteWrite(int type, bool uidRead, unsigned int uid);

// True when the stub must skip the store. Only an interior stitch is ever
// skipped, and only in guard mode; everything else keeps the vanilla write.
bool StitchByteSkipsWrite(StitchByteClass cls, bool actMode);

// --- Stub code generation ---------------------------------------------

bool StitchByteRel32Reaches(unsigned __int64 from, unsigned __int64 to);

const size_t kStitchByteStubLen = 0x3B;

// Writes the stub at `stubAddr`:
//
//   push rax, rcx, rdx, r8, r9, r10, r11   ; 7 pushes + 28h keeps rsp 16-aligned
//   sub  rsp, 28h
//   mov  rcx, rsi                          ; output
//   mov  edx, r15d                         ; task type
//   call qword ptr [gateSlotAddr]          ; __fastcall int(const void*, int)
//   add  rsp, 28h
//   test eax, eax
//   pop  r11, r10, r9, r8, rdx, rcx, rax   ; pops leave the flags alone
//   jz   .skip
//   mov  byte ptr [rsi+50h], 0             ; the two original instructions
// .skip:
//   mov  rax, [rsi+8]
//   jmp  resumeAddr
//
// The gate returns nonzero to keep the store. xmm0-5 need no save: nothing
// between the site and the next call reads them.
bool BuildStitchByteStub(unsigned char* out, size_t cap,
                         unsigned __int64 stubAddr, unsigned __int64 gateSlotAddr,
                         unsigned __int64 resumeAddr, size_t* outLen);

// mov eax,1 / ret -- the keep-every-store replacement for the gate slot.
const size_t kStitchByteKeepAllLen = 6;
bool BuildStitchByteKeepAll(unsigned char* out, size_t cap);

// The patched form of the site: `E9 rel32` to the stub and three 0x90. The
// eight bytes straddle an 8-byte boundary, so they cannot be written in one
// store; the installer only writes them before the path thread exists.
bool BuildStitchByteSitePatch(const unsigned char* current,
                              unsigned __int64 siteAddr, unsigned __int64 stubAddr,
                              unsigned char* out8);

#endif // KENSHI_ZONE_OPT_FIXES_STITCH_BYTE_GUARD_POLICY_H
