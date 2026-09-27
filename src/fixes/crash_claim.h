#ifndef KENSHI_ZONE_OPT_FIXES_CRASH_CLAIM_H
#define KENSHI_ZONE_OPT_FIXES_CRASH_CLAIM_H

#include <string>

// Pure logic behind claiming a leftover crash_dump.txt at plugin startup
// (plugin/crash_record.cpp). No Windows header, no I/O: kept host-testable under
// tools/tests/. The actual file rename lives in plugin/crash_record.cpp, next to the crash
// handler it protects.

// The two paths under one mod directory: the name the crash handler writes to
// during this session, and the name a file left over from an earlier session
// is moved to before this session's own g_crashFilePath is set.
std::string CrashDumpCurrentPath(const std::string& dllDir);
std::string CrashDumpPreviousPath(const std::string& dllDir);

// First-chance C++ throws go to their own file, so crash_dump.txt keeps
// meaning "the process faulted": a throw that something catches is a normal
// event, and writing it into the crash record would make the record's mere
// existence stop being evidence of a crash.
std::string CppExceptionDumpPath(const std::string& dllDir);

enum CrashClaimOutcome
{
	CRASH_CLAIM_NONE,    // no crash_dump.txt existed at startup: nothing to claim
	CRASH_CLAIM_RENAMED, // a leftover was found and moved out of the way
	CRASH_CLAIM_FAILED   // a leftover was found but the rename failed
};

// One fixed, greppable line for the mod's own log, given what the claim
// attempt found. Every startup logs exactly one of these, unconditionally, so
// a log missing all three means the claim never ran this session (the DLL
// crashed, or never reached this point, before the line could be written).
std::string CrashClaimLogLine(CrashClaimOutcome outcome, unsigned long lastError);

#endif // KENSHI_ZONE_OPT_FIXES_CRASH_CLAIM_H
