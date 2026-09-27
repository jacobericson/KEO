// nm_l2_writer.cpp — see nm_l2_writer.h
//
// Runs on the NavMesh bg thread and the worker threads: C file I/O, the Win32
// file API and Interlocked only, no CRT string objects and no logging. The
// caller owns the logging, because only it knows which thread it is on.

#include "navmesh/cache/nm_l2_writer.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static volatile long l2WrFail[L2WR_OUTCOME_COUNT] = {};
static volatile long l2WrLastErr      = 0;
static volatile long l2WrLastOutcome  = L2WR_OK;
static volatile long l2WrLoudLatch[L2WR_OUTCOME_COUNT] = {};
static volatile long l2NoBankLatch    = 0;


// --------------------------------------------------------------------
// The write
// --------------------------------------------------------------------

// Creating a directory that is already there is not a failure; nothing else is
// recoverable, and the caller retries the open either way so the real verdict
// comes from that open rather than from this return.
static void L2EnsureDir(const char* dir)
{
	if (dir && dir[0])
		CreateDirectoryA(dir, NULL);
}

L2WriteOutcome L2WriteFileAtomic(const char* dir, const char* destPath,
                                 const void* data, size_t size,
                                 unsigned long* osErr)
{
	if (osErr) *osErr = 0;

	if (!data || size == 0) return L2WR_BADBLOB;
	if (!destPath || !destPath[0]) return L2WR_NOPATH;
	if (strlen(destPath) >= MAX_PATH) return L2WR_NOPATH;

	// The thread id makes the temporary name unique per writer. Two threads can
	// finish duplicate jobs for one key at once; with a shared name the second
	// open could fail against the first's handle, or truncate it between the
	// first's close and rename. The renames still land on the same key, last one
	// wins, and each is a complete file. The name ends in ".tmp", so the
	// startup and cap sweeps remove leftovers by their "*.tmp" pattern.
	char tmpPath[MAX_PATH + 16];
	int n = _snprintf_s(tmpPath, sizeof(tmpPath), _TRUNCATE, "%s.%lx.tmp",
	                    destPath, (unsigned long)GetCurrentThreadId());
	if (n < 0) return L2WR_TMPPATH;
	// The buffer has room for the suffix but the ANSI file API does not: a
	// destination within ~13 characters of MAX_PATH yields a temporary name the
	// open below cannot use.
	if (strlen(tmpPath) >= MAX_PATH) return L2WR_TMPPATH;

	FILE* f = NULL;
	if (fopen_s(&f, tmpPath, "wb") != 0 || !f)
	{
		// A directory removed after startup is the one open failure worth
		// recovering from, and one attempt is the whole retry: a second would
		// buy nothing and this runs on a thread with a retire deadline.
		L2EnsureDir(dir);
		f = NULL;
		if (fopen_s(&f, tmpPath, "wb") != 0 || !f)
		{
			if (osErr) *osErr = (unsigned long)errno;
			return L2WR_OPEN;
		}
	}

	bool ok = (fwrite(data, size, 1, f) == 1);
	if (fflush(f) != 0) ok = false;
	int closeErr = errno;
	fclose(f);

	if (!ok)
	{
		if (osErr) *osErr = (unsigned long)closeErr;
		DeleteFileA(tmpPath);
		return L2WR_WRITE;
	}

	if (!MoveFileExA(tmpPath, destPath, MOVEFILE_REPLACE_EXISTING))
	{
		if (osErr) *osErr = GetLastError();
		DeleteFileA(tmpPath);
		return L2WR_RENAME;
	}
	return L2WR_OK;
}


// --------------------------------------------------------------------
// Accounting
// --------------------------------------------------------------------

bool L2WriteNote(L2WriteOutcome outcome, unsigned long osErr)
{
	if (outcome <= L2WR_OK || outcome >= L2WR_OUTCOME_COUNT)
		return false;

	InterlockedIncrement(&l2WrFail[outcome]);
	InterlockedExchange(&l2WrLastOutcome, (long)outcome);
	InterlockedExchange(&l2WrLastErr, (long)osErr);

	// One latch per cause, not one for the session: a benign first failure must
	// not spend the announcement a fatal later one needs. Nine lines is the
	// ceiling, so this still cannot become per-miss spam.
	return InterlockedCompareExchange(&l2WrLoudLatch[outcome], 1, 0) == 0;
}

long L2WriteFailTotal()
{
	long total = 0;
	for (int i = 1; i < L2WR_OUTCOME_COUNT; ++i)
		total += InterlockedCompareExchange(&l2WrFail[i], 0, 0);
	return total;
}

long L2WriteFailCount(int outcome)
{
	if (outcome < 0 || outcome >= L2WR_OUTCOME_COUNT) return 0;
	return InterlockedCompareExchange(&l2WrFail[outcome], 0, 0);
}

unsigned long L2WriteLastErr()
{ return (unsigned long)InterlockedCompareExchange(&l2WrLastErr, 0, 0); }

int L2WriteLastFailOutcome()
{ return (int)InterlockedCompareExchange(&l2WrLastOutcome, 0, 0); }

const char* L2WriteOutcomeName(int outcome)
{
	switch (outcome)
	{
	case L2WR_OK:       return "ok";
	case L2WR_BADENTRY: return "bad-entry";
	case L2WR_PAYLOAD:  return "payload-range";
	case L2WR_ALLOC:    return "alloc-failed";
	case L2WR_BADBLOB:  return "no-bytes";
	case L2WR_NOPATH:   return "no-path";
	case L2WR_TMPPATH:  return "temp-name-too-long";
	case L2WR_OPEN:     return "open-failed";
	case L2WR_WRITE:    return "write-failed";
	case L2WR_RENAME:   return "rename-failed";
	default:            return "unknown";
	}
}

void L2WriteFormatToken(char* out, size_t outSize)
{
	if (!out || outSize == 0) return;
	out[0] = 0;

	long total = L2WriteFailTotal();
	if (total == 0)
	{
		_snprintf_s(out, outSize, _TRUNCATE, " l2WrFail=0");
		return;
	}
	_snprintf_s(out, outSize, _TRUNCATE,
	            " l2WrFail=%ld(e%ld/l%ld/a%ld/b%ld/n%ld/t%ld/o%ld/w%ld/r%ld) l2WrErr=%lu(%s)",
	            total,
	            L2WriteFailCount(L2WR_BADENTRY),
	            L2WriteFailCount(L2WR_PAYLOAD),
	            L2WriteFailCount(L2WR_ALLOC),
	            L2WriteFailCount(L2WR_BADBLOB),
	            L2WriteFailCount(L2WR_NOPATH),
	            L2WriteFailCount(L2WR_TMPPATH),
	            L2WriteFailCount(L2WR_OPEN),
	            L2WriteFailCount(L2WR_WRITE),
	            L2WriteFailCount(L2WR_RENAME),
	            L2WriteLastErr(),
	            L2WriteOutcomeName(L2WriteLastFailOutcome()));
}

bool L2NoBankFirstReport()
{ return InterlockedCompareExchange(&l2NoBankLatch, 1, 0) == 0; }

void L2WriteStatsResetForTest()
{
	for (int i = 0; i < L2WR_OUTCOME_COUNT; ++i)
		InterlockedExchange(&l2WrFail[i], 0);
	InterlockedExchange(&l2WrLastErr, 0);
	InterlockedExchange(&l2WrLastOutcome, L2WR_OK);
	for (int j = 0; j < L2WR_OUTCOME_COUNT; ++j)
		InterlockedExchange(&l2WrLoudLatch[j], 0);
	InterlockedExchange(&l2NoBankLatch, 0);
}
