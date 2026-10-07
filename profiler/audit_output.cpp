// audit_output.cpp - Queued logging and reporter output.
// The reporter owns every file; fallback writes run on callers before startup.
// QueueLine and DrainLines take g_lineCS alone; the main thread publishes the frame ring.

#include "audit_detail.h"
#include "audit_steady.h"
#include "base/ini_names.h"
#include <stdarg.h>
#include <stdio.h>

namespace kenshiframeaudit_detail {
// =========================================================================
// Output plumbing: line queue + frame ring, drained by the reporter thread
// =========================================================================

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

CRITICAL_SECTION        g_lineCS;
bool                    g_lineCSReady = false;
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
HANDLE                  g_wake     = NULL;
HANDLE                  g_reporter = NULL;
volatile LONG           g_reporterRunning = 0;

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
FrameRec*       g_ring = NULL;
volatile LONG64 g_ringHead = 0;                // next slot the main thread fills
volatile LONG64 g_ringTail = 0;                // next slot the reporter reads
volatile LONG   g_ringLost = 0;

std::string ProfilerLogPath()
{
	return g_dllDir + PROFILER_LOG_NAME;
}

// Fallback when the reporter thread is not running (before Audit_Init, or if
// it failed to start): append synchronously.
void WriteDirect(int target, const std::string& text)
{
	if (target != LOG_PROFILER || g_dllDir.empty())
		return;
	FILE* f = NULL;
	if (fopen_s(&f, ProfilerLogPath().c_str(), "a") == 0 && f)
	{
		fputs(text.c_str(), f);
		fputc('\n', f);
		fclose(f);
	}
}

void QueueLine(int target, const std::string& text)
{
	if (!g_reporterRunning || !g_lineCSReady)
	{
		WriteDirect(target, text);
		return;
	}
	QueuedLine q;
	q.target = target;
	q.text   = text;
	EnterCriticalSection(&g_lineCS);
	g_lines.push_back(q);
	LeaveCriticalSection(&g_lineCS);
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

// Audit lines carry the same "seconds: " prefix as the profiler log.
void AuditLine(const std::string& text)
{
	char prefix[32];
	_snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "%.3f: ", SinceStart(Now()));
	QueueLine(LOG_AUDIT, std::string(prefix) + text);
}

} // audit


namespace kenshiframeaudit_detail {

void RingPush(const FrameRec& r)
{
	if (!g_ring)
		return;
	LONG64 head = g_ringHead;
	LONG64 tail = InterlockedCompareExchange64(&g_ringTail, 0, 0);
	if (head - tail >= RING_SIZE)
	{
		InterlockedIncrement(&g_ringLost);
		return;
	}
	g_ring[head & (RING_SIZE - 1)] = r;
	_WriteBarrier();
	InterlockedExchange64(&g_ringHead, head + 1);
}

bool RingPop(FrameRec* out)
{
	LONG64 tail = g_ringTail;
	LONG64 head = InterlockedCompareExchange64(&g_ringHead, 0, 0);
	if (tail >= head)
		return false;
	_ReadBarrier();
	*out = g_ring[tail & (RING_SIZE - 1)];
	InterlockedExchange64(&g_ringTail, tail + 1);
	return true;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

// =========================================================================
// Reporter thread: files, timer measurement, formatting helpers
// =========================================================================

TimerInfo g_timer;

// renderSingleObject / _setPass are timed with RDTSC (a few ns) instead of
// QPC; the reporter keeps the tick rate calibrated against QPC over the run.
LONGLONG           g_calQ0 = 0;
unsigned long long g_calT0 = 0;

void CalibrateTsc()
{
	if (!g_calQ0)
	{
		g_calQ0 = Now();
		g_calT0 = __rdtsc();
		return;
	}
	double ms = (double)(Now() - g_calQ0) * 1000.0 / (double)g_qpcFreq;
	if (ms >= 100.0)
		g_tscPerMs = (double)(__rdtsc() - g_calT0) / ms;
}

TimerInfo MeasureTimer()
{
	TimerInfo ti;
	memset(&ti, 0, sizeof(ti));

	typedef LONG (WINAPI *NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	NtQueryTimerResolution_t query = ntdll
		? (NtQueryTimerResolution_t)GetProcAddress(ntdll, "NtQueryTimerResolution") : NULL;
	if (query)
	{
		ULONG coarse = 0, fine = 0, cur = 0;
		if (query(&coarse, &fine, &cur) == 0)
		{
			ti.resOk       = true;
			ti.resCoarseMs = coarse / 10000.0;
			ti.resFineMs   = fine / 10000.0;
			ti.resCurMs    = cur / 10000.0;
		}
	}

	double sum = 0.0, mx = 0.0;
	for (int i = 0; i < 10; ++i)
	{
		LONGLONG a = Now();
		Sleep(1);
		double ms = TicksToMs(Now() - a);
		sum += ms;
		if (ms > mx) mx = ms;
	}
	ti.sleep1AvgMs = sum / 10.0;
	ti.sleep1MaxMs = mx;

	LONGLONG a = Now();
	for (int i = 0; i < 1000; ++i)
		Now();
	ti.qpcNs = (double)TicksToMs(Now() - a) * 1000.0;   // ms per 1000 calls -> ns per call
	return ti;
}

void WriteRaw(FILE* f, const std::string& s)
{
	if (!f)
		return;
	fputs(s.c_str(), f);
	fputc('\n', f);
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

std::string Fmt(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	return std::string(buf);
}

} // audit


namespace kenshiframeaudit_detail {

// Value or "-" when not measured.
std::string V(float v, int decimals)
{
	if (IsNan(v))
		return "-";
	char buf[32];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.*f", decimals, v);
	return std::string(buf);
}

void AuditOut(const std::string& text)
{
	char prefix[32];
	_snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "%.3f: ", SinceStart(Now()));
	WriteRaw(g_auditLog ? g_auditLog : g_profLog, std::string(prefix) + text);
}

void WriteTimerLine()
{
	std::string s = "[AUDIT-TIMER]";
	if (g_timer.resOk)
		s += Fmt(" res cur=%.3fms fine=%.3fms coarse=%.3fms",
		         g_timer.resCurMs, g_timer.resFineMs, g_timer.resCoarseMs);
	else
		s += " res=unavailable";
	s += Fmt(" sleep1 avg=%.2fms max=%.2fms qpc=%.0fns timerExp=%d",
	         g_timer.sleep1AvgMs, g_timer.sleep1MaxMs, g_timer.qpcNs,
	         g_cfg.timerExperiment ? 1 : 0);
	AuditOut(s);
}

void OpenAuditFiles()
{
	std::string dir = g_dllDir + "audit\\";
	CreateDirectoryA(dir.c_str(), NULL);
	std::string stem = dir + g_runName;
	fopen_s(&g_auditLog, (stem + ".log").c_str(), "w");
	if (g_cfg.csvSeconds)
		fopen_s(&g_secCsv, (stem + "_sec.csv").c_str(), "w");
	if (g_cfg.csvFrames)
		fopen_s(&g_frameCsv, (stem + "_frames.csv").c_str(), "w");
	if (g_cfg.physxDetail)
	{
		fopen_s(&g_physCsv,  (stem + "_phys.csv").c_str(), "w");
		fopen_s(&g_physqCsv, (stem + "_physq.csv").c_str(), "w");
	}
	if (g_cfg.cpuSample)
		fopen_s(&g_cpuCsv, (stem + "_cpu.csv").c_str(), "w");
	if (g_auditLog) setvbuf(g_auditLog, NULL, _IOFBF, 64 * 1024);
	if (g_secCsv)   setvbuf(g_secCsv,   NULL, _IOFBF, 64 * 1024);
	if (g_frameCsv) setvbuf(g_frameCsv, NULL, _IOFBF, 256 * 1024);
	if (g_physCsv)  setvbuf(g_physCsv,  NULL, _IOFBF, 64 * 1024);
	if (g_physqCsv) setvbuf(g_physqCsv, NULL, _IOFBF, 64 * 1024);
	if (g_cpuCsv)   setvbuf(g_cpuCsv,   NULL, _IOFBF, 64 * 1024);
}

void FlushFiles()
{
	if (g_profLog)  fflush(g_profLog);
	if (g_auditLog) fflush(g_auditLog);
	if (g_secCsv)   fflush(g_secCsv);
	if (g_frameCsv) fflush(g_frameCsv);
	if (g_physCsv)  fflush(g_physCsv);
	if (g_physqCsv) fflush(g_physqCsv);
	if (g_cpuCsv)   fflush(g_cpuCsv);
}

void DrainLines()
{
	std::vector<QueuedLine> batch;
	EnterCriticalSection(&g_lineCS);
	batch.swap(g_lines);
	LeaveCriticalSection(&g_lineCS);
	for (size_t i = 0; i < batch.size(); ++i)
	{
		FILE* f = (batch[i].target == LOG_AUDIT && g_auditLog) ? g_auditLog : g_profLog;
		WriteRaw(f, batch[i].text);
	}
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
unsigned __stdcall ReporterProc(void*)
{
	fopen_s(&g_profLog, ProfilerLogPath().c_str(), "a");
	if (g_profLog) setvbuf(g_profLog, NULL, _IOFBF, 64 * 1024);
	ResetSecond(-1);
	CalibrateTsc();
	if (g_cfg.enabled)
	{
		OpenAuditFiles();
		g_timer = MeasureTimer();
		WriteTimerLine();
	}

	LONGLONG lastFlush = Now();
	FrameRec rec;
	for (;;)
	{
		Sleep(100);
		CalibrateTsc();
		DrainLines();
		while (RingPop(&rec))
			OnRecord(rec);
		CpuSampleTick();
		LONGLONG now = Now();
		if (now - lastFlush >= g_qpcFreq)
		{
			FlushFiles();
			lastFlush = now;
		}
	}
	// not reached: the thread lives until the process exits
}


} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
