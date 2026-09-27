// Drives the navmesh worker retire loop with a real thread standing in for a
// worker that will not exit. The loop waits on the thread's handle through its
// wait operation; the clock is a counter this harness advances, so the 45 s cap
// is reached without waiting on real time. The run shows:
//
//   1. stall: a worker held in its generation for the whole budget gets eight
//      slice lines, the final record and one terminate with exit code 3, while
//      its thread is still live;
//   2. late: a worker that ends its generation, then its cleanup, past the
//      report threshold is joined without a terminate, its phase read as it is;
//   3. failed: a wait that fails is charged a slice, and the loop proceeds only
//      once the live count reads 0;
//   4. loglock: with the log's lock held by another thread, one bounded take is
//      refused and every line goes to the fallback file instead.
//
// Every scheduling point is an event handshake bounded by WATCHDOG_MS; one that
// runs out is a failure, never a pass. Links src/navmesh/workers/nm_retire_policy.cpp
// and src/base/log_bounded.cpp unchanged.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "navmesh/workers/nm_retire_policy.h"
#include "base/log_bounded.h"

#include "check.h"

namespace retire_stall_injection_detail
{
	const DWORD WATCHDOG_MS = 10000;

	// ---- The worker -------------------------------------------------------------

	struct Worker
	{
		HANDLE        thread;
		HANDLE        releaseGen;      // manual-reset: the generation may end
		HANDLE        releaseCleanup;  // manual-reset: the cleanup may end
		HANDLE        phaseSet;        // auto-reset: the worker published its phase
		volatile LONG phase;           // 0 generating, 1 storing
		DWORD         genWait, cleanupWait;
	};

	static DWORD WINAPI WorkerProc(LPVOID p)
	{
		Worker* w = (Worker*)p;
		InterlockedExchange(&w->phase, 0);
		SetEvent(w->phaseSet);
		w->genWait = WaitForSingleObject(w->releaseGen, WATCHDOG_MS);
		InterlockedExchange(&w->phase, 1);
		SetEvent(w->phaseSet);
		w->cleanupWait = WaitForSingleObject(w->releaseCleanup, WATCHDOG_MS);
		return 0;
	}

	// Starts the worker and consumes its first phase signal, so the phase the
	// harness reads next is the generating one and the next signal is the change.
	void StartWorker(Worker* w, const char* tag)
	{
		memset(w, 0, sizeof(*w));
		w->genWait        = WAIT_FAILED;
		w->cleanupWait    = WAIT_FAILED;
		w->releaseGen     = CreateEvent(NULL, TRUE, FALSE, NULL);
		w->releaseCleanup = CreateEvent(NULL, TRUE, FALSE, NULL);
		w->phaseSet       = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (w->releaseGen && w->releaseCleanup && w->phaseSet)
			w->thread = CreateThread(NULL, 0, WorkerProc, w, 0, NULL);
		char what[128];
		_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the worker started and published its phase", tag);
		Check(w->thread != NULL && WaitForSingleObject(w->phaseSet, WATCHDOG_MS) == WAIT_OBJECT_0, what);
	}

	bool ThreadLive(HANDLE h)
	{
		return h != NULL && WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	}

	// Releases both of the worker's waits, joins it and closes its handles. Its
	// two waits must have ended on the harness's events, not on their watchdogs.
	void FinishWorker(Worker* w, const char* tag)
	{
		if (w->releaseGen)
			SetEvent(w->releaseGen);
		if (w->releaseCleanup)
			SetEvent(w->releaseCleanup);
		char what[128];
		if (w->thread)
		{
			const DWORD joined = WaitForSingleObject(w->thread, WATCHDOG_MS);
			_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the worker thread joined", tag);
			Check(joined == WAIT_OBJECT_0, what);
			_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the harness, not a watchdog, released the worker", tag);
			Check(joined == WAIT_OBJECT_0 && w->genWait == WAIT_OBJECT_0 && w->cleanupWait == WAIT_OBJECT_0, what);
			CloseHandle(w->thread);
		}
		if (w->releaseGen)
			CloseHandle(w->releaseGen);
		if (w->releaseCleanup)
			CloseHandle(w->releaseCleanup);
		if (w->phaseSet)
			CloseHandle(w->phaseSet);
		memset(w, 0, sizeof(*w));
	}

	// ---- The harness and the loop's operations ------------------------------------

	enum Act { ACT_NONE = 0, ACT_END_GEN, ACT_END_CLEANUP, ACT_END_ALL };

	const int MAX_WAITS = 16;
	const int MAX_LINES = 16;

	struct Harness
	{
		const char*       tag;
		Worker*           w;
		HANDLE            handles[2];
		DWORD             n;
		unsigned          clock;
		unsigned          joinAdvance;
		Act               script[MAX_WAITS];   // the action before wait i + 1
		int               waits;
		CRITICAL_SECTION* heldLock;            // NULL: the log's lock is free
		const char*       fallbackPath;        // NULL: no fallback file
		int               logCalls;
		int               fallbackCalls;
		int               terminateCalls;
		unsigned          terminateCode;
		unsigned          terminateClock;
		bool              terminateThreadLive;
		int               lineCount;           // lines stored, capped at MAX_LINES
		char              lines[MAX_LINES][RETIRE_LINE_CHARS];
	};

	void InitHarness(Harness* h, const char* tag, Worker* w)
	{
		memset(h, 0, sizeof(*h));
		h->tag        = tag;
		h->w          = w;
		h->handles[0] = w->thread;
		h->n          = 1;
	}

	unsigned OpNowMs(void* ctx)
	{
		return ((Harness*)ctx)->clock;
	}

	int OpLiveCount(void* ctx)
	{
		Harness* h = (Harness*)ctx;
		int live = 0;
		for (DWORD i = 0; i < h->n; ++i)
			if (WaitForSingleObject(h->handles[i], 0) == WAIT_TIMEOUT)
				++live;
		return live;
	}

	RetireWait OpWaitSlice(void* ctx, unsigned ms, unsigned long* gle)
	{
		Harness* h = (Harness*)ctx;
		const int index = h->waits++;
		const Act act = index < MAX_WAITS ? h->script[index] : ACT_NONE;
		char what[128];
		if (index == MAX_WAITS)
		{
			// Past the longest script: fail once and let the worker go, so a loop
			// that still reads the live count proceeds and the output stays bounded.
			_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: the loop ran past sixteen waits", h->tag);
			Check(false, what);
			SetEvent(h->w->releaseGen);
			SetEvent(h->w->releaseCleanup);
			WaitForSingleObject(h->w->thread, WATCHDOG_MS);
		}
		if (act == ACT_END_GEN || act == ACT_END_ALL)
		{
			SetEvent(h->w->releaseGen);
			_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: wait %d: the worker published its storing phase", h->tag, index + 1);
			Check(WaitForSingleObject(h->w->phaseSet, WATCHDOG_MS) == WAIT_OBJECT_0, what);
		}
		if (act == ACT_END_CLEANUP || act == ACT_END_ALL)
		{
			SetEvent(h->w->releaseCleanup);
			_snprintf_s(what, sizeof(what), _TRUNCATE, "%s: wait %d: the worker thread exited", h->tag, index + 1);
			Check(WaitForSingleObject(h->w->thread, WATCHDOG_MS) == WAIT_OBJECT_0, what);
		}
		const DWORD r = WaitForMultipleObjects(h->n, h->handles, TRUE, 0);
		if (r < WAIT_OBJECT_0 + h->n)
		{
			h->clock += h->joinAdvance;
			return RETIRE_WAIT_JOINED;
		}
		if (r == WAIT_TIMEOUT)
		{
			h->clock += ms;
			return RETIRE_WAIT_TIMEOUT;
		}
		*gle = GetLastError();
		return RETIRE_WAIT_FAILED;
	}

	long OpStopDrops(void*)
	{
		return 0;
	}

	void OpPhases(void* ctx, char* out, size_t cap)
	{
		Harness* h = (Harness*)ctx;
		if (cap == 0)
			return;
		out[0] = 0;
		if (ThreadLive(h->w->thread))
			_snprintf_s(out, cap, _TRUNCATE, "w0:%s",
			            InterlockedCompareExchange(&h->w->phase, 0, 0) == 0 ? "generating" : "storing");
	}

	bool OpLog(void* ctx, const char* line)
	{
		Harness* h = (Harness*)ctx;
		++h->logCalls;
		if (h->heldLock)
		{
			if (!EnterCriticalSectionBounded(h->heldLock, 50))
				return false;
			LeaveCriticalSection(h->heldLock);
		}
		std::printf("retire_stall_injection: %s: %s\n", h->tag, line);
		if (h->lineCount < MAX_LINES)
			strcpy_s(h->lines[h->lineCount++], RETIRE_LINE_CHARS, line);
		return true;
	}

	void OpLogFallback(void* ctx, const char* line)
	{
		Harness* h = (Harness*)ctx;
		++h->fallbackCalls;
		if (!h->fallbackPath)
			return;
		char text[RETIRE_LINE_CHARS + 2];
		_snprintf_s(text, sizeof(text), _TRUNCATE, "%s\r\n", line);
		AppendLineRaw(h->fallbackPath, text, strlen(text));
	}

	void OpTerminate(void* ctx, unsigned code)
	{
		Harness* h = (Harness*)ctx;
		++h->terminateCalls;
		h->terminateCode       = code;
		h->terminateClock      = h->clock;
		h->terminateThreadLive = ThreadLive(h->w->thread);
	}

	RetireOps MakeOps(Harness* h)
	{
		RetireOps ops;
		ops.ctx         = h;
		ops.nowMs       = &OpNowMs;
		ops.liveCount   = &OpLiveCount;
		ops.waitSlice   = &OpWaitSlice;
		ops.stopDrops   = &OpStopDrops;
		ops.phases      = &OpPhases;
		ops.log         = &OpLog;
		ops.logFallback = &OpLogFallback;
		ops.terminate   = &OpTerminate;
		return ops;
	}

	// The standing line the retire writes once the loop has returned.
	void EmitRetiredLine(Harness* h, const RetireOps& ops, RetireResult* r)
	{
		RetireSummary s;
		s.activeCount   = 1;
		s.waitMs        = r->waitMs;
		s.anyWaitFailed = r->anyWaitFailed;
		s.lastGle       = r->lastGle;
		s.live          = OpLiveCount(h);
		s.stopDrop      = 0;
		s.cleanupLeft   = 0;
		char line[RETIRE_LINE_CHARS];
		RetireFormatRetiredLine(line, sizeof(line), s);
		RetireEmit(ops, &r->logStuck, line);
	}

	// ---- The scenarios ---------------------------------------------------------------

	Worker  g_worker;
	Harness g_h;
	int     g_stallLineCount = 0;
	char    g_stallLines[MAX_LINES][RETIRE_LINE_CHARS];

	void Stall()
	{
		const int before = CheckFailureCount();
		StartWorker(&g_worker, "stall");
		InitHarness(&g_h, "stall", &g_worker);
		const RetireOps ops = MakeOps(&g_h);
		const RetireResult r = RetireRun(ops);

		Check(g_h.waits == 9, "stall: nine waits");
		Check(g_h.terminateCalls == 1 && r.terminated, "stall: one terminate");
		Check(g_h.terminateCode == RETIRE_EXIT_CODE && g_h.terminateCode == 3, "stall: the terminate's exit code is 3");
		Check(g_h.terminateClock == 45000, "stall: the terminate comes at 45000 ms");
		Check(g_h.terminateThreadLive, "stall: the worker thread is still live at the terminate");
		Check(g_h.lineCount == 9, "stall: eight slice lines and the final record");
		Check(g_h.lineCount == 9 && strncmp(g_h.lines[8], "RETIRE TERMINATE: exit code 3,", 30) == 0,
		      "stall: the last line is the final record");

		g_stallLineCount = g_h.lineCount;
		memcpy(g_stallLines, g_h.lines, sizeof(g_stallLines));
		const unsigned code  = g_h.terminateCode;
		const unsigned clock = g_h.terminateClock;
		FinishWorker(&g_worker, "stall");
		if (CheckFailureCount() == before)
			std::printf("retire_stall_injection: stall: terminate recorded code=%u at %u ms, the worker thread still held\n",
			            code, clock);
	}

	void Late()
	{
		StartWorker(&g_worker, "late");
		InitHarness(&g_h, "late", &g_worker);
		g_h.script[3]   = ACT_END_GEN;
		g_h.script[4]   = ACT_END_CLEANUP;
		g_h.joinAdvance = 700;
		const RetireOps ops = MakeOps(&g_h);
		RetireResult r = RetireRun(ops);
		const int sliceLines = g_h.lineCount;
		EmitRetiredLine(&g_h, ops, &r);

		Check(g_h.terminateCalls == 0 && !r.terminated, "late: no terminate");
		Check(g_h.waits == 5, "late: five waits");
		Check(r.waitMs == 20700, "late: the retire's wait time is 20700 ms");
		Check(sliceLines == 4, "late: four slice lines");
		Check(sliceLines >= 4 && strstr(g_h.lines[3], "phases=[w0:storing]") != NULL,
		      "late: the fourth slice line reads the storing phase");
		FinishWorker(&g_worker, "late");
	}

	void Failed()
	{
		StartWorker(&g_worker, "failed");
		InitHarness(&g_h, "failed", &g_worker);
		g_h.script[1] = ACT_END_ALL;
		const RetireOps ops = MakeOps(&g_h);
		// Nothing may create a handle between this close and the loop's return,
		// or the closed value could name a live object again.
		HANDLE closed = CreateEvent(NULL, TRUE, FALSE, NULL);
		Check(closed != NULL, "failed: the event to close was created");
		CloseHandle(closed);
		g_h.handles[1] = closed;
		g_h.n          = 2;
		RetireResult r = RetireRun(ops);
		EmitRetiredLine(&g_h, ops, &r);

		Check(r.anyWaitFailed, "failed: a wait failed");
		Check(r.lastGle == ERROR_INVALID_HANDLE, "failed: the last error is ERROR_INVALID_HANDLE");
		Check(g_h.waits == 2, "failed: two waits");
		Check(g_h.terminateCalls == 0 && !r.terminated, "failed: no terminate");
		Check(g_h.clock == 0, "failed: the clock never advanced");
		Check(g_h.lineCount == 2 && strstr(g_h.lines[0], "retireSlice=1 joined=FAILED") != NULL,
		      "failed: the failed wait was charged a slice");
		FinishWorker(&g_worker, "failed");
	}

	struct Holder
	{
		CRITICAL_SECTION* lock;
		HANDLE            held;
		HANDLE            release;
		DWORD             releaseWait;
	};

	DWORD WINAPI HolderProc(LPVOID p)
	{
		Holder* k = (Holder*)p;
		EnterCriticalSection(k->lock);
		SetEvent(k->held);
		k->releaseWait = WaitForSingleObject(k->release, WATCHDOG_MS);
		LeaveCriticalSection(k->lock);
		return 0;
	}

	// The whole file, or false when it cannot be read or exceeds cap - 1 bytes.
	bool ReadFileBytes(const char* path, char* out, size_t cap, size_t* len)
	{
		*len = 0;
		HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		                       FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD got = 0;
		const BOOL ok = ReadFile(f, out, (DWORD)(cap - 1), &got, NULL);
		CloseHandle(f);
		if (!ok || got >= cap - 1)
			return false;
		out[got] = 0;
		*len = got;
		return true;
	}

	char g_expected[MAX_LINES * (RETIRE_LINE_CHARS + 2) + 1];
	char g_read[MAX_LINES * (RETIRE_LINE_CHARS + 2) + 1];

	void LogLock()
	{
		const int before = CheckFailureCount();
		const char* path = "build\\tests\\inj\\retire_stall_injection\\fallback.txt";
		DeleteFileA(path);

		CRITICAL_SECTION lock;
		InitializeCriticalSection(&lock);
		Holder k;
		k.lock        = &lock;
		k.held        = CreateEvent(NULL, FALSE, FALSE, NULL);
		k.release     = CreateEvent(NULL, TRUE, FALSE, NULL);
		k.releaseWait = WAIT_FAILED;
		HANDLE holder = NULL;
		if (k.held && k.release)
			holder = CreateThread(NULL, 0, HolderProc, &k, 0, NULL);
		Check(holder != NULL && WaitForSingleObject(k.held, WATCHDOG_MS) == WAIT_OBJECT_0,
		      "loglock: the holder took the log's lock");

		StartWorker(&g_worker, "loglock");
		InitHarness(&g_h, "loglock", &g_worker);
		g_h.heldLock     = &lock;
		g_h.fallbackPath = path;
		const RetireOps ops = MakeOps(&g_h);
		const RetireResult r = RetireRun(ops);
		FinishWorker(&g_worker, "loglock");

		if (k.release)
			SetEvent(k.release);
		if (holder)
		{
			Check(WaitForSingleObject(holder, WATCHDOG_MS) == WAIT_OBJECT_0 && k.releaseWait == WAIT_OBJECT_0,
			      "loglock: the harness released the holder and joined it");
			CloseHandle(holder);
		}
		if (k.held)
			CloseHandle(k.held);
		if (k.release)
			CloseHandle(k.release);

		Check(g_h.logCalls == 1, "loglock: the one bounded take was refused");
		Check(r.logStuck, "loglock: the log is marked stuck");
		Check(g_h.fallbackCalls == 9, "loglock: nine lines reached the fallback");
		Check(g_h.terminateCalls == 1 && r.terminated, "loglock: one terminate");

		g_expected[0] = 0;
		for (int i = 0; i < g_stallLineCount; ++i)
		{
			strcat_s(g_expected, sizeof(g_expected), g_stallLines[i]);
			strcat_s(g_expected, sizeof(g_expected), "\r\n");
		}
		size_t len = 0;
		const bool read = ReadFileBytes(path, g_read, sizeof(g_read), &len);
		Check(read, "loglock: the fallback file was read back");
		Check(g_stallLineCount == 9 && read && len == strlen(g_expected) && memcmp(g_read, g_expected, len) == 0,
		      "loglock: the fallback file holds the stall's nine lines");

		const bool taken = EnterCriticalSectionBounded(&lock, 50);
		Check(taken, "loglock: the lock is taken once the holder has gone");
		if (taken)
			LeaveCriticalSection(&lock);
		DeleteCriticalSection(&lock);

		if (CheckFailureCount() == before)
			std::printf("retire_stall_injection: loglock: the log take was refused once and %d lines reached the fallback file, equal to the stall's\n",
			            g_h.fallbackCalls);
	}
}

using namespace retire_stall_injection_detail;

int main()
{
	Stall();
	Late();
	Failed();
	LogLock();
	return CheckExit("retire_stall_injection");
}
