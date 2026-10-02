// exit_capture_policy.h -- pure logic behind the DEV exit-capture instrument.
// No Windows header, no memory walking: the record text, file paths and
// arming rules are host-testable, exactly like fatal_class.h and
// crash_claim.h. The Windows-touching half (the detours, the dump thread,
// RtlCaptureStackBackTrace, MiniDumpWriteDump) lives in exit_capture.cpp,
// which is not host-tested.

#ifndef KEO_DIAG_EXIT_CAPTURE_POLICY_H
#define KEO_DIAG_EXIT_CAPTURE_POLICY_H

#include <stddef.h>
#include <string>
#include "diag/module_bases.h"

// Which detour actually fired. Printed into the record so a reader knows
// whether the exit code came from a fabricated EXCEPTION_POINTERS (UEF, the
// CRT's own direct call) or from the call every termination path funnels
// through (the ntdll choke point).
enum ExitCaptureSource
{
	EXITCAP_SOURCE_UEF = 0,       // kernel32/KERNELBASE!UnhandledExceptionFilter
	EXITCAP_SOURCE_TERMINATE = 1  // ntdll!NtTerminateProcess
};

const char* ExitCaptureSourceToken(int source);

// True only for the exception codes the CRT hands to UnhandledExceptionFilter
// without ever raising a real exception: a security-check or fail-fast abort
// (0xC0000409), an invalid-parameter abort (0xC0000417), and the abort() exit
// message (0x40000015). Every other code reaching the UEF detour is a
// genuinely unhandled exception the VEH and RE_Kenshi's own handler already
// see; arming for those would race RE_Kenshi's dump, emergency save and
// report window with a second one.
bool ExitCaptureUefCodeArmable(unsigned long code);

// Whether the instrument should treat a death as the one it exists for.
// `stopHookInstalled` is false for a session that never got far enough to
// install the lazy NavMesh::stop hook (a main-menu-only quit, for one);
// `stopSeen` is true once the game has begun its own normal teardown;
// `anyCrashRecorded` is true once crash_dump.txt already has a record this
// session (an AV, div-by-zero or stack overflow already diagnosed and shown
// to the player). All three must hold for the instrument to arm.
bool ExitCaptureShouldArm(bool stopHookInstalled, bool stopSeen, bool anyCrashRecorded);

// One "EXIT" record: the source, the exit/exception code, the calling
// thread, and a return-address stack walk already captured by the caller
// (RtlCaptureStackBackTrace touches the live stack -- nothing to fake here)
// resolved through FormatAddrMod. `imageBases[i]` is the VirtualQuery
// AllocationBase for frames[i] (0 if unknown or not looked up), the same
// per-frame fallback EmitCrashRecord uses for an address outside the
// snapshot table; pass NULL to skip that fallback entirely (still valid,
// just less precise for a late-loaded module). Same no-CRT, fixed-buffer
// style as EmitCrashRecord (plugin/crash_record.cpp). Returns the number of bytes written.
size_t ExitCaptureFormatRecord(char* out, size_t cap, int source,
                                unsigned long code, unsigned long tid,
                                const unsigned __int64* frames,
                                const unsigned __int64* imageBases, int frameCount,
                                const ModuleBaseEntry* mods, int modCount, bool modsValid);

// Paths under the mod's own directory, next to crash_dump.txt. Neither name
// carries a timestamp: each has exactly one ".prev" sibling, claimed at
// install the same way crash_dump.txt claims its own leftover, so a launch
// never accumulates one file per session.
std::string ExitCaptureTextPath(const std::string& dllDir);
std::string ExitCaptureTextPrevPath(const std::string& dllDir);
std::string ExitCaptureMinidumpPath(const std::string& dllDir);
std::string ExitCaptureMinidumpPrevPath(const std::string& dllDir);

// The line logged once at install, naming which pieces of plumbing came up
// (a hook target found and its prologue verified, the dump thread started,
// dbghelp resolved). None of this means the instrument can fire yet -- see
// ExitCaptureArmedLogLine / ExitCaptureDisarmedLogLine below. Absent parts
// print '?' rather than being left out, so a log missing this line entirely
// (the DLL crashed before install) is distinguishable from one where a piece
// is present but reads '?'.
std::string ExitCaptureInstallLogLine(bool uefInstalled, bool terminateInstalled,
                                       bool dumpThreadOk, bool dbghelpOk);

// Logged once the gate actually opens (the NavMesh::stop hook installs):
// whether the instrument can really fire this session, given what installed.
// If neither OS hook came up, this reads as a disarmed line ("no OS hook
// installed") rather than an "armed" line naming two things that are both
// off -- there is nothing armed to report.
std::string ExitCaptureArmedLogLine(bool uefInstalled, bool terminateInstalled);

// Logged once, only if the NavMesh::stop hook's own install failed or never
// ran: names the reason the instrument can never arm this session.
std::string ExitCaptureDisarmedLogLine();

// A file this instrument writes to, next to crash_dump.txt: whether a
// leftover from a previous session was there to claim, and what happened.
// NONE covers both "nothing there" and "there, but empty" -- an empty
// leftover carries no evidence, so it is silently reclaimed by the
// CREATE_ALWAYS that follows rather than preserved as a ".prev" file.
enum ExitCaptureClaimOutcome
{
	EXITCAP_CLAIM_NONE = 0,  // nothing worth rotating; this session opens the file normally
	EXITCAP_CLAIM_ROTATED,   // a non-empty leftover was renamed to its ".prev" sibling
	EXITCAP_CLAIM_FAILED     // a non-empty leftover exists but the rename failed
};

// What a EXITCAP_CLAIM_FAILED outcome means for the rest of the session: the
// unrotated leftover is kept exactly as it is, and this instrument does not
// open (or write to) that file at all this session, rather than overwrite a
// record the rotation was supposed to preserve. `lastError` is the
// GetLastError() value from the failed MoveFileExA, always printed even when
// 0 (a caller-observed race, not the syscall's own failure).
std::string ExitCaptureClaimFailedToken(unsigned long lastError);

#endif // KEO_DIAG_EXIT_CAPTURE_POLICY_H
