#include <cstdio>
#include "fixes/crash_claim.h"

#include "check.h"

int main()
{
	Check(CrashDumpCurrentPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\crash_dump.txt",
	      "current path is dllDir + crash_dump.txt");
	Check(CrashDumpPreviousPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\crash_dump.prev.txt",
	      "previous path is dllDir + crash_dump.prev.txt");
	Check(CrashDumpCurrentPath("") == "crash_dump.txt", "an empty dir still names the file");

	// Every outcome logs a fixed "CrashClaim: " line so a collector can grep
	// a run's log for exactly one of the three, never zero and never guess.
	std::string none = CrashClaimLogLine(CRASH_CLAIM_NONE, 0);
	std::string renamed = CrashClaimLogLine(CRASH_CLAIM_RENAMED, 0);
	std::string failed = CrashClaimLogLine(CRASH_CLAIM_FAILED, 5);

	Check(none.find("CrashClaim: ") == 0, "the NONE line starts with the marker");
	Check(renamed.find("CrashClaim: ") == 0, "the RENAMED line starts with the marker");
	Check(failed.find("CrashClaim: ") == 0, "the FAILED line starts with the marker");

	Check(none != renamed && none != failed && renamed != failed,
	      "the three outcomes never read as the same line");

	Check(failed.find("5") != std::string::npos, "a failed rename's GetLastError reaches the line");
	Check(CrashClaimLogLine(CRASH_CLAIM_FAILED, 0).find("GetLastError=0") != std::string::npos,
	      "GetLastError=0 is still printed, not swallowed as falsy");

	return CheckExit("crash_claim_units");
}
