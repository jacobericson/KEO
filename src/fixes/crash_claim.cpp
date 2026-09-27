#include "fixes/crash_claim.h"
#include <sstream>

std::string CrashDumpCurrentPath(const std::string& dllDir)
{
	return dllDir + "crash_dump.txt";
}

std::string CrashDumpPreviousPath(const std::string& dllDir)
{
	return dllDir + "crash_dump.prev.txt";
}

std::string CppExceptionDumpPath(const std::string& dllDir)
{
	return dllDir + "cpp_exception_dump.txt";
}

std::string CrashClaimLogLine(CrashClaimOutcome outcome, unsigned long lastError)
{
	switch (outcome)
	{
	case CRASH_CLAIM_RENAMED:
		return "CrashClaim: renamed an earlier session's crash_dump.txt to "
		       "crash_dump.prev.txt at startup; this session has not run yet";
	case CRASH_CLAIM_FAILED:
		{
			std::ostringstream msg;
			msg << "CrashClaim: found an earlier session's crash_dump.txt but "
			       "could not rename it at startup (GetLastError=" << lastError
			    << "); its presence later cannot be trusted as this session's own record";
			return msg.str();
		}
	case CRASH_CLAIM_NONE:
	default:
		return "CrashClaim: no leftover crash_dump.txt found at startup";
	}
}
