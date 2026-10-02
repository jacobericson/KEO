// log.cpp - Log output and the deferred log queue.
// Any thread; initialization and deferred flush run on the main thread.
// LogMsgBounded holds logCS while FlushDeferredLogLines takes pendingLogCS
// for a bounded copy: logCS -> pendingLogCS. LogMsg then re-enters logCS
// on the same thread after pendingLogCS is released.

#include "base/core.h"
#include "base/core_internal.h"
#include "base/ini_names.h"
#include "base/log_bounded.h"
#include "fixes/world/destroy_list_defer.h"

using namespace core_detail;

// =========================================================================
// Log utilities
// =========================================================================

static CRITICAL_SECTION logCS;
static bool logCSInitialized = false;
static char s_retireFallbackPath[MAX_PATH] = { 0 };
static DWORD mainThreadId = 0;

// The deferred log-line queue (LogMsgDeferrable, far below). Initialised in
// InitLogFile for the same reason as deferCS.
static CRITICAL_SECTION pendingLogCS;
static bool pendingLogCSInitialized = false;

std::string GetDLLDirectory()
{
	char path[MAX_PATH];
	HMODULE hm = NULL;
	if (!GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&GetDLLDirectory, &hm))
	{
		return std::string();
	}
	GetModuleFileNameA(hm, path, sizeof(path));
	std::string dir(path);
	size_t pos = dir.find_last_of("\\/");
	if (pos != std::string::npos)
		dir = dir.substr(0, pos + 1);
	return dir;
}

void InitLogFile()
{
	logFilePath = GetDLLDirectory() + OPTIMIZER_LOG_NAME;
	// Shares the log's lifetime: present only if this session wrote it.
	const std::string retirePath = GetDLLDirectory() + OPTIMIZER_RETIRE_NAME;
	if (retirePath.size() < sizeof(s_retireFallbackPath))
	{
		memcpy(s_retireFallbackPath, retirePath.c_str(), retirePath.size() + 1);
		DeleteFileA(s_retireFallbackPath);
	}
	if (!logCSInitialized)
	{
		InitializeCriticalSection(&logCS);
		logCSInitialized = true;
		mainThreadId = GetCurrentThreadId();
	}
	DestroyListDeferInit();
	if (!pendingLogCSInitialized)
	{
		InitializeCriticalSection(&pendingLogCS);
		pendingLogCSInitialized = true;
	}
}

bool IsMainThread()
{
	return mainThreadId != 0 && GetCurrentThreadId() == mainThreadId;
}

static void OpenLogFile()
{
	if (logFile.is_open() || logFilePath.empty())
		return;

	logFile.open(logFilePath.c_str(), std::ios::trunc);

#ifdef KEO_DEBUG
	// LogMsg calls flush() per line, but that only hands the stream buffer to
	// the OS; making the DEV stream unbuffered removes that buffer entirely, so
	// each line reaches the OS inside the write that produced it and nothing is
	// left in user space for a crashing thread to lose. PROD keeps the buffered
	// stream and the per-line flush.
	//
	// This must come AFTER the open, not before it. MSVC 2010's
	// basic_filebuf::setbuf returns failure immediately when _Myfile == 0, so on
	// an unopened stream it does nothing at all; once the file is open it reaches
	// setvbuf(_Myfile, NULL, _IONBF, 0), which is the call that matters. Nothing
	// has been written to the stream yet at this point, which setvbuf requires.
	if (logFile.is_open())
		logFile.rdbuf()->pubsetbuf(0, 0);
#endif
}

static void LogLine(std::ofstream& file, const std::string& line)
{
	// Only call DebugLog on main thread (RE_Kenshi thread safety unknown)
	if (GetCurrentThreadId() == mainThreadId)
		DebugLog(line);

	std::ostringstream ts;
	ts << std::fixed << std::setprecision(3) << ElapsedSec() << ": " << line;
	file << ts.str() << "\n";
}

void LogMsg(const std::string& line)
{
	if (!logCSInitialized)
		return;
	EnterCriticalSection(&logCS);
	OpenLogFile();
	if (logFile.is_open())
	{
		LogLine(logFile, line);
		logFile.flush();
	}
	LeaveCriticalSection(&logCS);
}

#ifdef KEO_DEBUG
void LogDebug(const std::string& line)
{
	LogMsg(line);
}
#endif


// =========================================================================
// Deferred log lines (off the main thread) — see core.h
// =========================================================================
//
// Fixed storage, no allocation: a producer copies its line under
// pendingLogCS, and the main thread takes the whole batch under the same lock
// and logs it with the lock released. pendingLogCS is a leaf: nothing is
// called while it is held but a bounded copy.

static const int DEFERRED_LOG_LINES = 32;
static char s_pendingLog[DEFERRED_LOG_LINES][DEFERRED_LOG_CHARS];
static int  s_pendingLogCount = 0;
static volatile LONG s_pendingLogDropped = 0;

// Main thread only (FlushDeferredLogLines), so a plain static is enough and
// the batch costs no stack.
static char s_flushLog[DEFERRED_LOG_LINES][DEFERRED_LOG_CHARS];

void LogMsgDeferrable(const char* line)
{
	if (!line)
		return;
	if (IsMainThread() || !pendingLogCSInitialized)
	{
		// On the main thread, or before InitLogFile (which is the main
		// thread's too): log now.
		LogMsg(line);
		return;
	}

	// Where and when it happened, since it is written a frame later.
	char stamped[DEFERRED_LOG_CHARS];
	_snprintf_s(stamped, sizeof(stamped), _TRUNCATE, "%s [deferred: tid=%lu t=%.3f]",
		line, (unsigned long)GetCurrentThreadId(), ElapsedSec());

	bool stored = false;
	EnterCriticalSection(&pendingLogCS);
	if (s_pendingLogCount < DEFERRED_LOG_LINES)
	{
		memcpy(s_pendingLog[s_pendingLogCount], stamped, sizeof(stamped));
		s_pendingLogCount++;
		stored = true;
	}
	LeaveCriticalSection(&pendingLogCS);

	if (!stored)
		InterlockedIncrement(&s_pendingLogDropped);
}

void FlushDeferredLogLines()
{
	if (!IsMainThread() || !pendingLogCSInitialized)
		return;

	int n = 0;
	EnterCriticalSection(&pendingLogCS);
	if (s_pendingLogCount > 0)
	{
		n = s_pendingLogCount;
		memcpy(s_flushLog, s_pendingLog, (size_t)n * DEFERRED_LOG_CHARS);
		s_pendingLogCount = 0;
	}
	LeaveCriticalSection(&pendingLogCS);

	for (int i = 0; i < n; ++i)
		LogMsg(s_flushLog[i]);

	LONG dropped = InterlockedExchange(&s_pendingLogDropped, 0);
	if (dropped > 0)
	{
		char line[128];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"Deferred log: %ld line(s) dropped (queue of %d full)",
			(long)dropped, DEFERRED_LOG_LINES);
		LogMsg(line);
	}
}

bool LogMsgBounded(const char* line, unsigned boundMs)
{
	if (!logCSInitialized || !line)
		return false;
	if (!EnterCriticalSectionBounded(&logCS, boundMs))
		return false;
	FlushDeferredLogLines();   // under logCS: LogMsg re-enters it, pendingLogCS is taken inside
	LogMsg(line);
	LeaveCriticalSection(&logCS);
	return true;
}

void LogRetireFallback(const char* line)
{
	if (!s_retireFallbackPath[0] || !line)
		return;
	char buf[512];
	const int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f: %s\r\n", ElapsedSec(), line);
	AppendLineRaw(s_retireFallbackPath, buf, n < 0 ? strlen(buf) : (size_t)n);
}
