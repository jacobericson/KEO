#include <cstdio>
#include <cstring>
#include "diag/exit_capture_policy.h"

#include "check.h"

int main()
{
	Check(std::string(ExitCaptureSourceToken(EXITCAP_SOURCE_UEF)) == "UEF",
	      "UEF source token");
	Check(std::string(ExitCaptureSourceToken(EXITCAP_SOURCE_TERMINATE)) == "NtTerminateProcess",
	      "terminate source token");
	Check(std::string(ExitCaptureSourceToken(99)) == "?", "an unknown source prints ? rather than garbage");

	// Only the CRT's fabricated-code bypass may arm the UEF path. Every
	// code in, armed or not out -- including every code the VEH itself
	// recognizes, which must never also arm the UEF detour.
	Check(ExitCaptureUefCodeArmable(0xC0000409UL), "fail-fast arms");
	Check(ExitCaptureUefCodeArmable(0xC0000417UL), "invalid-parameter arms");
	Check(ExitCaptureUefCodeArmable(0x40000015UL), "abort() exit message arms");
	Check(!ExitCaptureUefCodeArmable(0xC0000005UL), "an access violation never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0xC0000094UL), "a divide by zero never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0xC00000FDUL), "a stack overflow never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0xE06D7363UL), "a C++ throw never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0xC0000374UL), "heap corruption never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0x80000003UL), "a breakpoint never arms the UEF path");
	Check(!ExitCaptureUefCodeArmable(0), "code 0 never arms the UEF path");

	// The arming gate: all three conditions must hold.
	Check(ExitCaptureShouldArm(true, false, false), "the one combination that arms");
	Check(!ExitCaptureShouldArm(false, false, false), "no stop-hook install means no arm");
	Check(!ExitCaptureShouldArm(true, true, false), "a normal quit in progress means no arm");
	Check(!ExitCaptureShouldArm(true, false, true), "an already-recorded crash means no arm");
	Check(!ExitCaptureShouldArm(false, true, true), "every condition failing still means no arm");

	// Paths sit next to crash_dump.txt, under the mod's own directory, and
	// each has exactly one ".prev" sibling -- no timestamp, so a launch never
	// accumulates one file per session.
	Check(ExitCaptureTextPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\exit_capture.txt",
	      "text path is dllDir + exit_capture.txt");
	Check(ExitCaptureTextPrevPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\exit_capture.prev.txt",
	      "text prev path is dllDir + exit_capture.prev.txt");
	Check(ExitCaptureMinidumpPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\exit_capture.dmp",
	      "dump path is dllDir + exit_capture.dmp, no timestamp");
	Check(ExitCaptureMinidumpPrevPath("C:\\mods\\ZoneOpt\\") == "C:\\mods\\ZoneOpt\\exit_capture.prev.dmp",
	      "dump prev path is dllDir + exit_capture.prev.dmp");

	// The record: no frames.
	{
		char line[512];
		size_t n = ExitCaptureFormatRecord(line, sizeof(line), EXITCAP_SOURCE_TERMINATE,
			0xC0000417UL, 4242, NULL, NULL, 0, NULL, 0, false);
		std::string s(line, n);
		Check(s.find("EXIT: source=NtTerminateProcess") == 0, "record opens with source=");
		Check(s.find("code=0xC0000417") != std::string::npos, "record carries the exit code in hex");
		Check(s.find("tid=4242") != std::string::npos, "record carries the calling thread id");
		Check(s.find("no stack") != std::string::npos, "zero frames says so instead of an empty walk");
	}

	// The record: frames resolved against a small module table.
	{
		ModuleBaseEntry mods[1];
		std::memset(mods, 0, sizeof(mods));
		std::strcpy(mods[0].name, "kenshi_x64.exe");
		mods[0].base = 0x140000000ULL;
		mods[0].size = 0x2000000ULL;

		unsigned __int64 frames[2] = { 0x140012345ULL, 0x7FF800001000ULL };
		unsigned __int64 imageBases[2] = { 0, 0x7FF800000000ULL };

		char line[1024];
		size_t n = ExitCaptureFormatRecord(line, sizeof(line), EXITCAP_SOURCE_UEF,
			0xC0000409UL, 7, frames, imageBases, 2, mods, 1, true);
		std::string s(line, n);
		Check(s.find("kenshi_x64.exe+0x12345") != std::string::npos,
		      "a frame inside the known module resolves to name+offset");
		Check(s.find("image@0x7FF800000000") != std::string::npos,
		      "a frame outside the table but inside a known image falls back to image@base+off");
	}

	// The install line says what came up, not whether it can fire yet.
	{
		std::string all = ExitCaptureInstallLogLine(true, true, true, true);
		std::string none = ExitCaptureInstallLogLine(false, false, false, false);
		Check(all.find("uef=installed") != std::string::npos, "an installed uef hook reads installed");
		Check(none.find("uef=?") != std::string::npos, "an uninstalled uef hook reads ?");
		Check(none.find("terminate=?") != std::string::npos, "an uninstalled terminate hook reads ?");
		Check(none.find("dumpThread=?") != std::string::npos, "no dump thread reads ?");
		Check(none.find("dbghelp=?") != std::string::npos, "no dbghelp reads ?");
		Check(all != none, "a fully installed line never reads the same as a fully uninstalled one");
		Check(all.find("armed") == std::string::npos,
		      "the install line never claims the instrument can already fire");
	}

	// The armed/disarmed lines: distinguishable from each other and from the
	// install line's own wording.
	{
		std::string armed = ExitCaptureArmedLogLine(true, true);
		std::string disarmed = ExitCaptureDisarmedLogLine();
		Check(armed.find("armed") != std::string::npos, "the armed line says armed");
		Check(armed != disarmed, "armed and disarmed never read the same");
		Check(ExitCaptureArmedLogLine(true, false) != ExitCaptureArmedLogLine(false, true),
		      "which hook actually installed is distinguishable in the armed line");

		// Both OS hooks failed: nothing is actually armed, so this must not
		// claim "armed (uef=off terminate=off)".
		std::string bothOff = ExitCaptureArmedLogLine(false, false);
		Check(bothOff.find("armed (") == std::string::npos,
		      "when neither OS hook installed, the line never claims armed");
		Check(bothOff.find("no OS hook installed") != std::string::npos,
		      "when neither OS hook installed, the line says so plainly");
	}

	// A failed claim keeps the unrotated leftover: the token names the error
	// and never claims success.
	{
		std::string token = ExitCaptureClaimFailedToken(5);
		Check(token.find("kept-prev") != std::string::npos, "a failed claim reads kept-prev");
		Check(token.find("err=5") != std::string::npos, "a failed claim's GetLastError reaches the token");
		Check(ExitCaptureClaimFailedToken(0).find("err=0") != std::string::npos,
		      "err=0 is still printed, not swallowed as falsy");
	}

	return CheckExit("exit_capture_units");
}
