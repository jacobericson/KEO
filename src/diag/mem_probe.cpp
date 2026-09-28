#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <string>

#include "diag/mem_probe.h"
#include "diag/cpp_exception.h"

// core.h's logger, declared rather than included so this file needs no game
// headers and the host tests can link it against a stub.
void LogMsg(const std::string& line);

namespace mem_probe_detail {

// PROCESS_MEMORY_COUNTERS_EX, restated so this file needs neither psapi.h nor
// psapi.lib. PrivateUsage is the process commit charge.
struct ProcessMemoryCountersEx
{
	DWORD  cb;
	DWORD  PageFaultCount;
	SIZE_T PeakWorkingSetSize;
	SIZE_T WorkingSetSize;
	SIZE_T QuotaPeakPagedPoolUsage;
	SIZE_T QuotaPagedPoolUsage;
	SIZE_T QuotaPeakNonPagedPoolUsage;
	SIZE_T QuotaNonPagedPoolUsage;
	SIZE_T PagefileUsage;
	SIZE_T PeakPagefileUsage;
	SIZE_T PrivateUsage;
};

typedef BOOL (WINAPI *PfnGetProcessMemoryInfo)(HANDLE, ProcessMemoryCountersEx*, DWORD);

static PfnGetProcessMemoryInfo g_getProcessMemoryInfo = 0;
static HANDLE                  g_selfProcess = 0;
static volatile LONG           g_initialised = 0;

// Main initialization and the periodic LogMemoryStats tick write sample and
// time between odd/even g_sampleSeq updates, then raise g_haveSample. Main
// reporters and any-thread crash readers use MemProbeLastSample: two checked
// copies, with failure reported as unavailable. A torn copy is rejected; the
// sample is diagnostic. Initialized once, never reset; a failed live query
// preserves the previous sample and timestamp.
static volatile LONG g_sampleSeq = 0;
static MemFigures    g_sample;
static double        g_sampleTime = -1.0;
static volatile LONG g_haveSample = 0;

static double g_lastLogTime = -1.0;
#ifdef ZONEOPT_DEBUG
// Matched to the cache stats line's cadence, so three consecutive rows that
// reprint one sample cannot read as flat memory.
const double LOG_INTERVAL_SEC = 10.0;
#else
const double LOG_INTERVAL_SEC = 30.0;
#endif

} // namespace
using namespace mem_probe_detail;

void MemProbeInit()
{
	if (InterlockedCompareExchange(&g_initialised, 1, 0) != 0)
		return;

	MemFiguresClear(&g_sample);
	g_selfProcess = GetCurrentProcess();

	// Resolved once, here, so the crash path never asks the loader for
	// anything. Windows 7 and later export the query from kernel32 itself;
	// psapi.dll is the fallback for nothing we support, but costs one line.
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	if (k32)
		g_getProcessMemoryInfo = (PfnGetProcessMemoryInfo)
			GetProcAddress(k32, "K32GetProcessMemoryInfo");
	if (!g_getProcessMemoryInfo)
	{
		HMODULE psapi = LoadLibraryA("psapi.dll");
		if (psapi)
			g_getProcessMemoryInfo = (PfnGetProcessMemoryInfo)
				GetProcAddress(psapi, "GetProcessMemoryInfo");
	}

	MemProbeSampleNow(0.0);
}

bool MemProbeRead(MemFigures* out)
{
	if (!out)
		return false;
	MemFiguresClear(out);

	if (g_getProcessMemoryInfo && g_selfProcess)
	{
		ProcessMemoryCountersEx pmc;
		memset(&pmc, 0, sizeof(pmc));
		pmc.cb = sizeof(pmc);
		if (g_getProcessMemoryInfo(g_selfProcess, &pmc, sizeof(pmc)))
		{
			out->privateCommit = (unsigned __int64)pmc.PrivateUsage;
			out->peakPrivate   = (unsigned __int64)pmc.PeakPagefileUsage;
			out->workingSet    = (unsigned __int64)pmc.WorkingSetSize;
			out->haveProcess   = 1;
		}
	}

	MEMORYSTATUSEX ms;
	memset(&ms, 0, sizeof(ms));
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms))
	{
		out->pagefileFree  = (unsigned __int64)ms.ullAvailPageFile;
		out->pagefileTotal = (unsigned __int64)ms.ullTotalPageFile;
		out->memLoadPct    = (unsigned long)ms.dwMemoryLoad;
		out->haveSystem    = 1;
	}

	return out->haveProcess || out->haveSystem;
}

void MemProbeSampleNow(double now)
{
	MemFigures f;
	if (!MemProbeRead(&f))
		return;

	InterlockedIncrement(&g_sampleSeq);
	g_sample = f;
	g_sampleTime = now;
	InterlockedIncrement(&g_sampleSeq);
	InterlockedExchange(&g_haveSample, 1);
}

bool MemProbeLastSample(MemFigures* out, double* ageSec, double now)
{
	if (!out)
		return false;
	MemFiguresClear(out);
	if (!InterlockedCompareExchange(&g_haveSample, 0, 0))
		return false;

	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG before = InterlockedCompareExchange(&g_sampleSeq, 0, 0);
		if (before & 1)
			continue;
		MemFigures f = g_sample;
		double when = g_sampleTime;
		if (InterlockedCompareExchange(&g_sampleSeq, 0, 0) != before)
			continue;
		*out = f;
		if (ageSec)
			*ageSec = now - when;
		return true;
	}
	return false;
}

size_t MemProbeShortLast(char* buf, size_t cap)
{
	MemFigures f;
	if (!MemProbeLastSample(&f, 0, 0.0))
	{
		MemFiguresClear(&f);
	}
	return MemFormatShort(buf, cap, f);
}

size_t MemProbeShortLastAged(char* buf, size_t cap, double now)
{
	if (!buf || cap == 0)
		return 0;
	buf[0] = '\0';

	MemFigures f;
	double age = 0.0;
	bool have = MemProbeLastSample(&f, &age, now);
	if (!have)
		MemFiguresClear(&f);

	size_t n = MemFormatShort(buf, cap, f);

	// The sampler and the line that borrows the sample run on their own
	// cadences, so consecutive rows can reprint one reading. The age says
	// which rows those are; without it flat figures read as flat memory.
	char tail[32];
	if (have)
	{
		long whole = (long)(age > 0.0 ? age : 0.0);
		_snprintf_s(tail, sizeof(tail), _TRUNCATE, " memSrc=last+%lds", whole);
	}
	else
	{
		_snprintf_s(tail, sizeof(tail), _TRUNCATE, " memSrc=none");
	}

	size_t t = 0;
	while (tail[t] && n + 1 < cap)
		buf[n++] = tail[t++];
	buf[n] = '\0';
	return n;
}

void LogMemoryStats(double now)
{
	if (g_lastLogTime >= 0.0 && now - g_lastLogTime < LOG_INTERVAL_SEC)
		return;
	g_lastLogTime = now;

	MemProbeSampleNow(now);

	MemFigures f;
	double age = 0.0;
	if (!MemProbeLastSample(&f, &age, now))
		MemFiguresClear(&f);

	char figures[256];
	MemFormatLong(figures, sizeof(figures), f);

	char cppEx[192];
	CppExceptionToken(cppEx, sizeof(cppEx));

	std::string line = "mem: ";
	line += figures;
	line += " ";
	line += cppEx;
	LogMsg(line);
}
