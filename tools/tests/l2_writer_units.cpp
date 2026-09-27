// Host tests for the L2 write path. Every failure cause that can be produced
// against a real directory is produced against a real directory: a missing
// destination, a path too long for the ANSI file API, a destination name
// already taken by a directory. The one cause with no portable injection (a
// short write) is checked at the outcome-to-counter mapping instead.

#include <cstdio>
#include <cstring>
#include <windows.h>
#include "navmesh/cache/nm_l2_writer.h"

#include "check.h"

static char g_root[MAX_PATH];

static void MakeRoot()
{
	char tmp[MAX_PATH];
	GetTempPathA(sizeof(tmp), tmp);
	_snprintf_s(g_root, sizeof(g_root), _TRUNCATE, "%skzo_l2_units_%lu\\",
	            tmp, (unsigned long)GetCurrentProcessId());
	CreateDirectoryA(g_root, NULL);
}

static void Join(char* out, size_t n, const char* leaf)
{ _snprintf_s(out, n, _TRUNCATE, "%s%s", g_root, leaf); }

static bool FileHas(const char* path, const char* bytes, size_t len)
{
	FILE* f = NULL;
	if (fopen_s(&f, path, "rb") != 0 || !f) return false;
	char buf[64] = {};
	size_t got = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	return got == len && memcmp(buf, bytes, len) == 0;
}

static int CountFiles(const char* dir, const char* pattern)
{
	char pat[MAX_PATH];
	_snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s%s", dir, pattern);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	int n = 0;
	do { ++n; } while (FindNextFileA(h, &fd));
	FindClose(h);
	return n;
}

static void RemoveTree(const char* dir)
{
	char pat[MAX_PATH];
	_snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s*", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
			char full[MAX_PATH];
			_snprintf_s(full, sizeof(full), _TRUNCATE, "%s%s", dir, fd.cFileName);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				char sub[MAX_PATH];
				_snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\", full);
				RemoveTree(sub);
			}
			else DeleteFileA(full);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryA(dir);
}


// --- the write, against real files ---------------------------------------

static void TestWriteOk()
{
	char dest[MAX_PATH];
	Join(dest, sizeof(dest), "ok.bin");
	DeleteFileA(dest);

	unsigned long err = 99;
	Check(L2WriteFileAtomic(g_root, dest, "hello", 5, &err) == L2WR_OK, "ok: outcome");
	Check(err == 0, "ok: no error reported");
	Check(FileHas(dest, "hello", 5), "ok: bytes landed");
	Check(CountFiles(g_root, "*.tmp") == 0, "ok: no temporary left behind");
}

static void TestWriteReplacesExisting()
{
	char dest[MAX_PATH];
	Join(dest, sizeof(dest), "ok.bin");
	Check(L2WriteFileAtomic(g_root, dest, "world!", 6, NULL) == L2WR_OK, "replace: outcome");
	Check(FileHas(dest, "world!", 6), "replace: new bytes won");
}

static void TestBadBlob()
{
	char dest[MAX_PATH];
	Join(dest, sizeof(dest), "bad.bin");
	Check(L2WriteFileAtomic(g_root, dest, NULL, 5, NULL) == L2WR_BADBLOB, "badblob: NULL data");
	Check(L2WriteFileAtomic(g_root, dest, "x", 0, NULL) == L2WR_BADBLOB, "badblob: zero size");
}

static void TestNoPath()
{
	Check(L2WriteFileAtomic(g_root, "", "x", 1, NULL) == L2WR_NOPATH, "nopath: empty");
	Check(L2WriteFileAtomic(g_root, NULL, "x", 1, NULL) == L2WR_NOPATH, "nopath: NULL");

	// A destination already past the ANSI limit is a lost path, not a lost
	// temporary: nothing can be built from it.
	char over[MAX_PATH + 40];
	memset(over, 'a', sizeof(over) - 1);
	over[sizeof(over) - 1] = 0;
	Check(L2WriteFileAtomic(g_root, over, "x", 1, NULL) == L2WR_NOPATH, "nopath: over MAX_PATH");
}

static void TestTmpPathTooLong()
{
	// Fits MAX_PATH itself, but not once ".<tid>.tmp" is appended.
	char nearLimit[MAX_PATH];
	memset(nearLimit, 'a', MAX_PATH - 2);
	nearLimit[MAX_PATH - 2] = 0;
	Check(strlen(nearLimit) < MAX_PATH, "tmppath: destination is inside the limit");
	Check(L2WriteFileAtomic(g_root, nearLimit, "x", 1, NULL) == L2WR_TMPPATH,
	      "tmppath: temporary name does not fit");
}

// The incident: the cache directory is gone after startup created it.
static void TestOpenRecoversDeletedDirectory()
{
	char dir[MAX_PATH], dest[MAX_PATH];
	_snprintf_s(dir, sizeof(dir), _TRUNCATE, "%sgone\\", g_root);
	CreateDirectoryA(dir, NULL);
	_snprintf_s(dest, sizeof(dest), _TRUNCATE, "%sfile.bin", dir);

	// Bank one file, then delete the directory under the writer.
	Check(L2WriteFileAtomic(dir, dest, "one", 3, NULL) == L2WR_OK, "recover: first write");
	DeleteFileA(dest);
	Check(RemoveDirectoryA(dir) != 0, "recover: directory removed");

	unsigned long err = 99;
	Check(L2WriteFileAtomic(dir, dest, "two", 3, &err) == L2WR_OK, "recover: write after removal");
	Check(err == 0, "recover: no error reported");
	Check(FileHas(dest, "two", 3), "recover: bytes landed");
	Check(GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES, "recover: directory recreated");
}

// A directory that cannot be created: the retry runs and the open still fails.
static void TestOpenFailsUnrecoverable()
{
	char dir[MAX_PATH], dest[MAX_PATH];
	// Two levels missing, so the single CreateDirectory cannot bridge it.
	_snprintf_s(dir, sizeof(dir), _TRUNCATE, "%sabsent\\deeper\\", g_root);
	_snprintf_s(dest, sizeof(dest), _TRUNCATE, "%sfile.bin", dir);

	unsigned long err = 0;
	Check(L2WriteFileAtomic(dir, dest, "x", 1, &err) == L2WR_OPEN, "open: unrecoverable");
	Check(err != 0, "open: error recorded");
	Check(GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES, "open: nothing created");
}

static void TestRenameFails()
{
	// The destination name is taken by a directory, so the rename onto it fails
	// after a good temporary has been written.
	char dest[MAX_PATH];
	Join(dest, sizeof(dest), "taken.bin");
	DeleteFileA(dest);
	Check(CreateDirectoryA(dest, NULL) != 0, "rename: destination is a directory");

	unsigned long err = 0;
	Check(L2WriteFileAtomic(g_root, dest, "x", 1, &err) == L2WR_RENAME, "rename: outcome");
	Check(err != 0, "rename: error recorded");
	Check(CountFiles(g_root, "*.tmp") == 0, "rename: temporary cleaned up");
	RemoveDirectoryA(dest);
}


// --- accounting -----------------------------------------------------------

static void TestCountersPerCause()
{
	L2WriteStatsResetForTest();

	Check(L2WriteNote(L2WR_OK, 0) == false, "counters: ok is not a failure");
	Check(L2WriteFailTotal() == 0, "counters: ok counts nothing");

	Check(L2WriteNote(L2WR_OPEN, 2) == true, "counters: first failure is loud");
	Check(L2WriteNote(L2WR_OPEN, 2) == false, "counters: a repeat of that cause is quiet");
	// A harmless first failure must not spend the announcement a fatal one needs.
	Check(L2WriteNote(L2WR_RENAME, 5) == true, "counters: a new cause is loud too");
	Check(L2WriteNote(L2WR_RENAME, 5) == false, "counters: and quiet thereafter");

	// The arm with no portable injection is proven here.
	Check(L2WriteNote(L2WR_WRITE, 28) == true, "counters: short write noted");

	Check(L2WriteFailCount(L2WR_OPEN) == 2, "counters: open arm");
	Check(L2WriteFailCount(L2WR_RENAME) == 2, "counters: rename arm");
	Check(L2WriteFailCount(L2WR_WRITE) == 1, "counters: write arm");
	Check(L2WriteFailCount(L2WR_BADBLOB) == 0, "counters: untouched arm stays zero");
	Check(L2WriteFailTotal() == 5, "counters: total");
	Check(L2WriteLastErr() == 28, "counters: last error");
	Check(L2WriteLastFailOutcome() == L2WR_WRITE, "counters: last cause");
}

static void TestEveryCauseHasItsOwnArm()
{
	// No two causes may share a slot: a reader has to be able to tell a missing
	// directory from a bad entry, because they need different responses.
	for (int a = 1; a < L2WR_OUTCOME_COUNT; ++a)
	{
		L2WriteStatsResetForTest();
		L2WriteNote((L2WriteOutcome)a, 1);
		for (int b = 1; b < L2WR_OUTCOME_COUNT; ++b)
			Check(L2WriteFailCount(b) == (a == b ? 1 : 0), "arms: one cause moves one counter");
		Check(strcmp(L2WriteOutcomeName(a), "unknown") != 0, "arms: cause is named");
	}
}

static void TestToken()
{
	char tok[256];

	L2WriteStatsResetForTest();
	L2WriteFormatToken(tok, sizeof(tok));
	Check(strcmp(tok, " l2WrFail=0") == 0, "token: clean session");

	Check(L2WriteNote(L2WR_OPEN, 3) == true, "token: first open is loud");
	Check(L2WriteNote(L2WR_OPEN, 3) == false, "token: repeat open is quiet");
	L2WriteNote(L2WR_BADENTRY, 0);
	L2WriteFormatToken(tok, sizeof(tok));
	Check(strstr(tok, " l2WrFail=3(") != NULL, "token: total");
	Check(strstr(tok, "o2") != NULL, "token: open arm printed");
	Check(strstr(tok, "e1") != NULL, "token: entry arm printed");
	Check(strstr(tok, "bad-entry") != NULL, "token: last cause named");

	// A short buffer truncates rather than overruns.
	char tiny[8];
	L2WriteFormatToken(tiny, sizeof(tiny));
	Check(strlen(tiny) < sizeof(tiny), "token: truncates safely");
}

static void TestNotBankingThreshold()
{
	Check(L2NotBanking(0, 0) == false, "nobank: an idle session is not an alarm");
	Check(L2NotBanking(L2_NOBANK_MISS_THRESHOLD - 1, 0) == false, "nobank: below the threshold");
	Check(L2NotBanking(L2_NOBANK_MISS_THRESHOLD, 0) == true, "nobank: at the threshold");
	Check(L2NotBanking(99, 0) == true, "nobank: the incident");
	Check(L2NotBanking(99, 1) == false, "nobank: one file banked clears it");

	L2WriteStatsResetForTest();
	Check(L2NoBankFirstReport() == true, "nobank: reported once");
	Check(L2NoBankFirstReport() == false, "nobank: not repeated");
}


int main()
{
	MakeRoot();

	TestWriteOk();
	TestWriteReplacesExisting();
	TestBadBlob();
	TestNoPath();
	TestTmpPathTooLong();
	TestOpenRecoversDeletedDirectory();
	TestOpenFailsUnrecoverable();
	TestRenameFails();

	TestCountersPerCause();
	TestEveryCauseHasItsOwnArm();
	TestToken();
	TestNotBankingThreshold();

	RemoveTree(g_root);

	return CheckExit("l2_writer_units");
}
