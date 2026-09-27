#include "diag/exit_capture_policy.h"
#include "base/fixed_log_buf.h"
#include <sstream>

namespace exit_capture_policy_detail
{
	typedef FlbExternal Buf;
}
using namespace exit_capture_policy_detail;

const char* ExitCaptureSourceToken(int source)
{
	switch (source)
	{
	case EXITCAP_SOURCE_UEF:       return "UEF";
	case EXITCAP_SOURCE_TERMINATE: return "NtTerminateProcess";
	default:                       return "?";
	}
}

bool ExitCaptureUefCodeArmable(unsigned long code)
{
	return code == 0xC0000409UL   // fail-fast / stack buffer overrun
		|| code == 0xC0000417UL   // CRT invalid-parameter abort
		|| code == 0x40000015UL;  // abort() exit message
}

bool ExitCaptureShouldArm(bool stopHookInstalled, bool stopSeen, bool anyCrashRecorded)
{
	return stopHookInstalled && !stopSeen && !anyCrashRecorded;
}

size_t ExitCaptureFormatRecord(char* out, size_t cap, int source,
                                unsigned long code, unsigned long tid,
                                const unsigned __int64* frames,
                                const unsigned __int64* imageBases, int frameCount,
                                const ModuleBaseEntry* mods, int modCount, bool modsValid)
{
	if (!out || cap == 0)
		return 0;

	Buf o;
	o.b = out;
	o.cap = cap;
	o.n = 0;

	FlbStr(&o, "EXIT: source=");
	FlbStr(&o, ExitCaptureSourceToken(source));
	FlbStr(&o, " code=0x");
	FlbHexDigits(&o, (unsigned __int64)code, 8);
	FlbStr(&o, " tid=");
	FlbDecU(&o, (unsigned __int64)tid);
	FlbStr(&o, "\r\n");

	for (int i = 0; i < frameCount && frames; ++i)
	{
		FlbStr(&o, "  #");
		FlbDecU(&o, (unsigned __int64)i);
		FlbStr(&o, " 0x");
		FlbHexDigits(&o, frames[i], 16);
		FlbChar(&o, ' ');
		char mod[48];
		unsigned __int64 imageBase = imageBases ? imageBases[i] : 0;
		size_t modLen = FormatAddrMod(mod, sizeof(mod), mods, modCount, modsValid, frames[i], imageBase);
		(void)modLen;
		FlbStr(&o, mod);
		FlbStr(&o, "\r\n");
	}
	if (frameCount == 0)
		FlbStr(&o, "  (no stack: RtlCaptureStackBackTrace returned nothing)\r\n");

	out[o.n < cap ? o.n : cap - 1] = '\0';
	return o.n;
}

std::string ExitCaptureTextPath(const std::string& dllDir)
{
	return dllDir + "exit_capture.txt";
}

std::string ExitCaptureTextPrevPath(const std::string& dllDir)
{
	return dllDir + "exit_capture.prev.txt";
}

std::string ExitCaptureMinidumpPath(const std::string& dllDir)
{
	return dllDir + "exit_capture.dmp";
}

std::string ExitCaptureMinidumpPrevPath(const std::string& dllDir)
{
	return dllDir + "exit_capture.prev.dmp";
}

std::string ExitCaptureInstallLogLine(bool uefInstalled, bool terminateInstalled,
                                       bool dumpThreadOk, bool dbghelpOk)
{
	std::ostringstream ss;
	ss << "ExitCapture: uef=" << (uefInstalled ? "installed" : "?")
	   << " terminate=" << (terminateInstalled ? "installed" : "?")
	   << " dumpThread=" << (dumpThreadOk ? "ok" : "?")
	   << " dbghelp=" << (dbghelpOk ? "ok" : "?");
	return ss.str();
}

std::string ExitCaptureArmedLogLine(bool uefInstalled, bool terminateInstalled)
{
	if (!uefInstalled && !terminateInstalled)
		return "ExitCapture: stays off this session (no OS hook installed)";
	std::ostringstream ss;
	ss << "ExitCapture: armed (uef=" << (uefInstalled ? "installed" : "off")
	   << " terminate=" << (terminateInstalled ? "installed" : "off") << ")";
	return ss.str();
}

std::string ExitCaptureDisarmedLogLine()
{
	return "ExitCapture: stays off this session (NavMesh::stop hook did not install)";
}

std::string ExitCaptureClaimFailedToken(unsigned long lastError)
{
	std::ostringstream ss;
	ss << "kept-prev(err=" << lastError << ")";
	return ss.str();
}
