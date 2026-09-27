#ifndef KENSHI_ZONE_OPT_NM_RETIRE_POLICY_H
#define KENSHI_ZONE_OPT_NM_RETIRE_POLICY_H

// The navmesh worker retire at NavMesh::stop: wait in bounded slices, never
// let the teardown run while a worker is live, and end the process
// deliberately at the cap. Pure: no Windows or game header. The loop reaches
// the clock, the workers, the log and the process only through RetireOps.

#include <stddef.h>

const unsigned RETIRE_SLICE_MS     = 5000;   // one wait, one record line
const unsigned RETIRE_REPORT_MS    = 15000;  // from here each line reads joined=HANG
const unsigned RETIRE_CAP_MS       = 45000;  // the final record, then the process ends
const unsigned RETIRE_LOG_BOUND_MS = 2000;   // the longest wait for the log's lock
const unsigned RETIRE_EXIT_CODE    = 3;
const size_t   RETIRE_LINE_CHARS   = 384;
const size_t   RETIRE_PHASES_CHARS = 128;

enum RetireAction
{
	RETIRE_PROCEED = 0,     // no worker is live: the teardown may run
	RETIRE_WAIT_SLICE,      // wait another slice
	RETIRE_REPORT_SLICE,    // wait another slice; its line is a report (a hang or a failed wait)
	RETIRE_TERMINATE        // write the final record and end the process
};

enum RetireWait { RETIRE_WAIT_JOINED = 0, RETIRE_WAIT_TIMEOUT, RETIRE_WAIT_FAILED };

// elapsedMs: the budget spent, never less than one slice per wait that did not
// end the retire. liveCount: workers whose threads have not exited.
// waitFailed: the last wait failed.
RetireAction RetireDecide(unsigned elapsedMs, int liveCount, bool waitFailed);

// The next wait: a slice, cut short at the cap; 0 at or past the cap.
unsigned RetireNextWaitMs(unsigned elapsedMs);

struct RetireLineInput
{
	RetireAction  action;     // the decision taken after the slice
	int           slice;      // retireSlice=
	unsigned      elapsedMs;  // waitMs=
	int           live;
	long          stopDrop;
	bool          waitFailed;
	unsigned long gle;
	const char*   phases;     // "w3:generating w5:building", or ""
};

struct RetireSummary
{
	int           activeCount;
	unsigned      waitMs;
	bool          anyWaitFailed;
	unsigned long lastGle;
	int           live;
	long          stopDrop;
	long          cleanupLeft;
};

// Each writes one NUL-terminated line, truncated to cap - 1, and returns its length.
size_t RetireFormatSliceLine(char* out, size_t cap, const RetireLineInput& in);
size_t RetireFormatTerminateLine(char* out, size_t cap, const RetireLineInput& in);
size_t RetireFormatRetiredLine(char* out, size_t cap, const RetireSummary& s);

struct RetireOps
{
	void*         ctx;
	unsigned    (*nowMs)(void* ctx);
	int         (*liveCount)(void* ctx);
	RetireWait  (*waitSlice)(void* ctx, unsigned ms, unsigned long* gle);
	long        (*stopDrops)(void* ctx);
	void        (*phases)(void* ctx, char* out, size_t cap);
	bool        (*log)(void* ctx, const char* line);          // false: the log's lock was not taken in time
	void        (*logFallback)(void* ctx, const char* line);  // lock-free
	void        (*terminate)(void* ctx, unsigned code);       // never returns in the DLL
};

struct RetireResult
{
	bool          terminated;     // set only when ops.terminate returned
	int           slices;         // slices spent
	unsigned      waitMs;         // the clock when the retire ended
	bool          anyWaitFailed;
	unsigned long lastGle;
	bool          logStuck;
};

// A line to the log, or to the fallback once the log has refused one.
void RetireEmit(const RetireOps& ops, bool* logStuck, const char* line);

// Returns once no worker is live. At the cap it writes the final record and
// calls ops.terminate, and returns (terminated set) only if that returns.
RetireResult RetireRun(const RetireOps& ops);

#endif
