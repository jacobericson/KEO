// audit_cpu.cpp - Per-thread CPU time. Once a second the reporter thread reads the cycle count of
// every thread of the process and writes one <run>_cpu.csv row per thread that ran since its last
// read, plus the process total and the part no followed thread accounts for; every 30 s an
// [AUDIT-THREADS] line names each thread's role and checks the cycle-to-ms rate against
// GetProcessTimes. The role notes run on the threads they name and write Interlocked words only;
// everything else here runs on the reporter thread.

#include "audit_steady.h"
#include <TlHelp32.h>

namespace audit_cpu_detail {

// A followed thread. The open handle keeps its id from being reused.
struct CpuThread
{
	DWORD   tid;
	HANDLE  h;
	ULONG64 last;          // cycles at the previous read
	char    module[40];    // module holding the start address, "-" when unknown
};

} // audit_cpu_detail
using namespace audit_cpu_detail;

namespace kenshiframeaudit_detail {

typedef BOOL (WINAPI *QueryCycles_t)(HANDLE, PULONG64);
typedef LONG (WINAPI *NtQueryThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static const int    CPU_MAX_THREADS     = 512;
static const int    CPU_MAX_OGRE        = 32;
static const double CPU_NAMES_SEC       = 30.0;
static const ULONG  CPU_START_ADDRESS   = 9;        // ThreadQuerySetWin32StartAddress
static const DWORD  CPU_QUERY_LIMITED   = 0x0800;   // THREAD_QUERY_LIMITED_INFORMATION
// Every Ogre worker thread starts at this wrapper (Threads::CreateThread hands it to
// ::CreateThread); its first bytes are checked before the address is trusted.
static const size_t        OGRE_WORKER_START = 0x2CD1E0;
static const unsigned char OGRE_WORKER_START_BYTES[16] = { 0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE };

static volatile LONG s_roleTid[CPU_ROLE_COUNT];
static volatile LONG s_ogreTid[CPU_MAX_OGRE];      // claimed from 0 by compare-exchange, never cleared

// Reporter thread only.
static CpuThread       s_threads[CPU_MAX_THREADS];
static int             s_count         = 0;
static int             s_dropped       = 0;      // threads the last snapshot found with the table full
static QueryCycles_t   s_threadCycles  = NULL;
static QueryCycles_t   s_processCycles = NULL;
static NtQueryThread_t s_queryThread   = NULL;
static bool            s_started       = false;
static bool            s_off           = false;
static LONGLONG        s_lastSample    = 0;
static LONGLONG        s_lastNames     = 0;
static ULONG64         s_procLast      = 0;
static ULONG64         s_timesLast     = 0;      // GetProcessTimes kernel + user, 100 ns units
static ULONG64         s_winProc       = 0;      // process cycles since the last names line
static ULONG64         s_winThreads    = 0;      // followed threads' cycles since then
static uintptr_t       s_ogreStart     = 0;        // 0: not checked or no match
static int             s_ogreByStart   = 0;        // threads named ogre by their start address

void CpuNoteRole(int role)
{
	if (role >= 0 && role < CPU_ROLE_COUNT)
		InterlockedExchange(&s_roleTid[role], (LONG)GetCurrentThreadId());
}

static void ClaimOgreTid(LONG tid)
{
	for (int i = 0; i < CPU_MAX_OGRE; ++i)
	{
		LONG v = s_ogreTid[i];
		if (v == tid)
			return;
		if (v == 0)
		{
			LONG prev = InterlockedCompareExchange(&s_ogreTid[i], tid, 0);
			if (prev == 0 || prev == tid)
				return;
		}
	}
}

void CpuNoteOgreWorker()
{
	ClaimOgreTid((LONG)GetCurrentThreadId());
}

static bool IsOgreWorker(DWORD tid)
{
	for (int i = 0; i < CPU_MAX_OGRE; ++i)
	{
		if ((DWORD)s_ogreTid[i] == tid)
			return true;
	}
	return false;
}

// The caller is the reporter, so its own id names the reporter.
static const char* RoleOf(DWORD tid)
{
	if (tid == g_mainThreadId)
		return "main";
	if (tid == GetCurrentThreadId())
		return "reporter";
	if (tid == (DWORD)s_roleTid[CPU_ROLE_AI] || tid == g_aiThreadId)
		return "ai";
	if (tid == (DWORD)s_roleTid[CPU_ROLE_PHYS])
		return "phys";
	if (tid == (DWORD)s_roleTid[CPU_ROLE_BIRDS])
		return "birds";
	if (IsOgreWorker(tid))
		return "ogre";
	return "other";
}

static void* StartModule(HANDLE h, char* out, size_t cap)
{
	strcpy_s(out, cap, "-");
	if (!s_queryThread)
		return NULL;
	PVOID start = NULL;
	if (s_queryThread(h, CPU_START_ADDRESS, &start, (ULONG)sizeof(start), NULL) != 0 || !start)
		return NULL;
	HMODULE mod = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCSTR)start, &mod) || !mod)
		return NULL;
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return NULL;
	const char* base = strrchr(path, '\\');
	strncpy_s(out, cap, base ? base + 1 : path, _TRUNCATE);
	return start;
}

static void FollowThread(DWORD tid)
{
	for (int i = 0; i < s_count; ++i)
	{
		if (s_threads[i].tid == tid)
			return;
	}
	if (s_count >= CPU_MAX_THREADS)
	{
		++s_dropped;
		return;
	}
	HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
	if (!h)
		h = OpenThread(CPU_QUERY_LIMITED, FALSE, tid);
	if (!h)
		return;
	ULONG64 c = 0;
	if (!s_threadCycles(h, &c))
	{
		CloseHandle(h);
		return;
	}
	CpuThread& th = s_threads[s_count++];
	th.tid  = tid;
	th.h    = h;
	th.last = c;
	void* start = StartModule(h, th.module, sizeof(th.module));
	if (s_ogreStart && (uintptr_t)start == s_ogreStart)
	{
		ClaimOgreTid((LONG)tid);
		++s_ogreByStart;
	}
}

static bool Snapshot()
{
	s_dropped = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	DWORD pid = GetCurrentProcessId();
	THREADENTRY32 te;
	memset(&te, 0, sizeof(te));
	te.dwSize = sizeof(te);
	if (Thread32First(snap, &te))
	{
		do
		{
			if (te.th32OwnerProcessID == pid)
				FollowThread(te.th32ThreadID);
			te.dwSize = sizeof(te);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	return true;
}

static ULONG64 ProcessTimes100ns()
{
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		return 0;
	ULONG64 k = ((ULONG64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
	ULONG64 u = ((ULONG64)user.dwHighDateTime << 32) | user.dwLowDateTime;
	return k + u;
}

static void WriteRow(double t, DWORD tid, const char* role, ULONG64 cycles, double perMs, double wallMs, const char* module)
{
	WriteRaw(g_cpuCsv, Fmt("%.3f,%lu,%s,%llu,%.3f,%.1f,%s", t, (unsigned long)tid, role,
	                       (unsigned long long)cycles, (double)cycles / perMs, wallMs, module));
}

static bool CpuStart(LONGLONG now)
{
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	HMODULE ntdll  = GetModuleHandleA("ntdll.dll");
	s_threadCycles  = kernel ? (QueryCycles_t)GetProcAddress(kernel, "QueryThreadCycleTime") : NULL;
	s_processCycles = kernel ? (QueryCycles_t)GetProcAddress(kernel, "QueryProcessCycleTime") : NULL;
	s_queryThread   = ntdll ? (NtQueryThread_t)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
	if (!s_threadCycles || !s_processCycles)
	{
		AuditOut("[Audit] cpu: off (QueryThreadCycleTime missing)");
		return false;
	}
	HMODULE ogre = GetModuleHandleA(OGRE_DLL);
	if (ogre && InModule(ogre, (const void*)((uintptr_t)ogre + OGRE_WORKER_START), sizeof(OGRE_WORKER_START_BYTES)) &&
	    memcmp((const void*)((uintptr_t)ogre + OGRE_WORKER_START), OGRE_WORKER_START_BYTES, sizeof(OGRE_WORKER_START_BYTES)) == 0)
		s_ogreStart = (uintptr_t)ogre + OGRE_WORKER_START;
	if (!Snapshot())
	{
		AuditOut(Fmt("[Audit] cpu: off (thread snapshot failed, error %lu)", (unsigned long)GetLastError()));
		return false;
	}
	s_processCycles(GetCurrentProcess(), &s_procLast);
	s_timesLast = ProcessTimes100ns();
	WriteRaw(g_cpuCsv, "t,tid,role,cycles,cpuMs,wallMs,module");
	AuditOut(Fmt("[Audit] cpu: on (%d threads, tscPerMs=%.0f, ogreStart=%s)", s_count, (double)g_tscPerMs, s_ogreStart ? "on" : "off"));
	s_lastSample = s_lastNames = now;
	s_started = true;
	return true;
}

static void WriteThreadNames(LONGLONG now, double perMs)
{
	double sec = (double)(now - s_lastNames) / (double)g_qpcFreq;
	if (sec <= 0.0)
		return;
	ULONG64 times = ProcessTimes100ns();
	double timesMs = times > s_timesLast ? (double)(times - s_timesLast) / 10000.0 : 0.0;
	s_timesLast = times;
	std::string line = Fmt("[AUDIT-THREADS] win=%.1fs threads=%d dropped=%d procMs/s=%.1f threadsMs/s=%.1f timesMs/s=%.1f tscPerMs=%.0f ogreByStart=%d |",
	                       sec, s_count, s_dropped, (double)s_winProc / perMs / sec,
	                       (double)s_winThreads / perMs / sec, timesMs / sec, perMs, s_ogreByStart);
	for (int i = 0; i < s_count; ++i)
	{
		const char* role = RoleOf(s_threads[i].tid);
		line += Fmt(" %lu=%s", (unsigned long)s_threads[i].tid, role);
		if (strcmp(role, "other") == 0)
			line += std::string(":") + s_threads[i].module;
	}
	AuditOut(line);
	s_winProc    = 0;
	s_winThreads = 0;
	s_lastNames  = now;
}

void CpuSampleTick()
{
	if (!g_cfg.cpuSample || !g_cpuCsv || s_off)
		return;
	double perMs = g_tscPerMs;
	if (perMs <= 0.0)
		return;
	LONGLONG now = Now();
	if (!s_started)
	{
		if (!CpuStart(now))
			s_off = true;
		return;
	}
	if (now - s_lastSample < g_qpcFreq)
		return;
	double wallMs = (double)(now - s_lastSample) * 1000.0 / (double)g_qpcFreq;
	double t = SinceStart(now);
	s_lastSample = now;
	Snapshot();   // new threads get their baseline read now and a row from the next sample on

	ULONG64 sum = 0;
	for (int i = 0; i < s_count; ++i)
	{
		CpuThread& th = s_threads[i];
		// Exit test first: the read after it is then the thread's last.
		DWORD code = 0;
		bool gone = !GetExitCodeThread(th.h, &code) || code != STILL_ACTIVE;
		ULONG64 c = 0;
		if (s_threadCycles(th.h, &c) && c > th.last)
		{
			ULONG64 d = c - th.last;
			th.last = c;
			sum += d;
			WriteRow(t, th.tid, RoleOf(th.tid), d, perMs, wallMs, th.module);
		}
		if (gone)
		{
			CloseHandle(th.h);
			s_threads[i] = s_threads[--s_count];
			--i;
		}
	}

	ULONG64 pc = 0;
	if (s_processCycles(GetCurrentProcess(), &pc))
	{
		ULONG64 pd = pc > s_procLast ? pc - s_procLast : 0;
		s_procLast = pc;
		WriteRow(t, 0, "process", pd, perMs, wallMs, "-");
		WriteRow(t, 0, "unattributed", pd > sum ? pd - sum : 0, perMs, wallMs, "-");
		s_winProc    += pd;
		s_winThreads += sum;
	}
	if ((double)(now - s_lastNames) >= CPU_NAMES_SEC * (double)g_qpcFreq)
		WriteThreadNames(now, perMs);
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
