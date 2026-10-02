// nm_l2_writer.h — L2 file write: outcomes, the atomic write itself, accounting
//
// Split out of nm_disk_cache.cpp so the write can be exercised against a real
// directory without the game: nothing here knows about navmesh entries, cache
// keys or the game's headers. Callers map their own failures onto the same
// outcome list, so one session token accounts for every reason an L2 file was
// not banked.

#ifndef KEO_NM_L2_WRITER_H
#define KEO_NM_L2_WRITER_H

#include <stddef.h>

// Why a generation did not reach disk. Everything but L2WR_OK loses the file.
// The letters are the ones the stats token prints.
enum L2WriteOutcome {
	L2WR_OK = 0,
	L2WR_BADENTRY,   // e: the L1 entry was inconsistent, so no bytes were built
	L2WR_PAYLOAD,    // l: the payload length was out of range
	L2WR_ALLOC,      // a: the blob buffer could not be allocated
	L2WR_BADBLOB,    // b: the write was handed no bytes
	L2WR_NOPATH,     // n: no destination path (cache directory unknown, or too long)
	L2WR_TMPPATH,    // t: the temporary name did not fit the ANSI file API
	L2WR_OPEN,       // o: the temporary file could not be opened
	L2WR_WRITE,      // w: the bytes did not reach the file
	L2WR_RENAME,     // r: the finished temporary could not take the final name
	L2WR_OUTCOME_COUNT
};

// Writes `size` bytes to "<destPath>.<tid>.tmp" and renames that over destPath,
// so a crash or a full disk never replaces a good file with a half-written one.
//
// `dir` is the directory destPath lives in, and may be NULL or empty. When the
// temporary cannot be opened it is created once and the open retried once: a
// cache directory removed after startup is the failure this recovers from, and
// one retry keeps a genuinely unwritable path from costing more than an open
// per write. `osErr` takes GetLastError()/errno for the failing step, else 0.
L2WriteOutcome L2WriteFileAtomic(const char* dir, const char* destPath,
                                 const void* data, size_t size,
                                 unsigned long* osErr);

// Records one outcome. L2WR_OK counts nothing. Returns true the first time each
// cause is seen in a session, which is the caller's cue to log: a cause that
// loses every file must announce itself even when something harmless failed
// first, and nine causes bound the lines.
bool L2WriteNote(L2WriteOutcome outcome, unsigned long osErr);

long          L2WriteFailTotal();
long          L2WriteFailCount(int outcome);
unsigned long L2WriteLastErr();
int           L2WriteLastFailOutcome();   // L2WR_OK when nothing has failed

// One word naming an outcome, for the log line.
const char* L2WriteOutcomeName(int outcome);

// " l2WrFail=0", or " l2WrFail=<total>(e../l../a../b../n../t../o../w../r..) l2WrErr=<last>".
// Always writes something, so a line without failures is not confusable with a
// line that failed to print the token. Truncates safely into outSize.
void L2WriteFormatToken(char* out, size_t outSize);

// A session that generated meshes and banked none of them. `misses` is the
// generation count, `diskWrites` the files that landed. Below the threshold an
// empty count is a legitimate transient: the write trails the job that produced
// it, so the first few generations of a session have not landed yet.
const long L2_NOBANK_MISS_THRESHOLD = 8;
inline bool L2NotBanking(long misses, long diskWrites)
{ return diskWrites == 0 && misses >= L2_NOBANK_MISS_THRESHOLD; }

// True the first time L2NotBanking holds, for the one-shot session alarm.
bool L2NoBankFirstReport();

void L2WriteStatsResetForTest();

#endif // KEO_NM_L2_WRITER_H
