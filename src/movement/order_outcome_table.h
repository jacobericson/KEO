#ifndef KENSHI_ZONE_OPT_ORDER_OUTCOME_TABLE_H
#define KENSHI_ZONE_OPT_ORDER_OUTCOME_TABLE_H

// The player order-outcome record table, pure: no game headers. A
// character is named by its raw pointer value, passed through as a size_t so
// this file need not know what a Character* is. order_outcome.cpp is the
// thin glue that reads the game and calls into this API; order_outcome_
// policy.h is the classifier and line formatters this file builds on.
//
// Not thread-aware by contract: every call happens on the main thread
// (order_outcome.cpp's callers all do), so there is no locking here either.
//
// Every function below that takes `now` also takes `paused` (default false,
// so the many pre-pause-fix call sites and host tests need no change). Every
// `now` is translated through one shared active-time clock before use: while
// paused, the clock does not advance, so a stall's duration, the 2s floor and
// the 600s safety timeout all measure unpaused time only. The translation is
// a single running total (ActiveTime, order_outcome_table.cpp), not a
// per-call flag check, so a stall that opens before a pause and resolves
// after it still measures correctly across the gap.

#include <string>
#include <cstddef>

// Where a finished "OrderOutcome:" line goes. Injected so the table has no
// LogMsg/game dependency; order_outcome.cpp binds it to LogMsg.
typedef void (*OotSink)(const std::string& line);

// Closes and prints every open record (end=reset), folding each into the
// session totals exactly like an ordinary close (a reload used to escape a
// stuck squad must still reach longFail). No `now`: a reload can land at any
// point mid-stall, so an open stall is just counted unrec, not measured
// (stallCharS/maxStallS are 0 for it -- there is no reliable "how long"
// without a live clock). Does NOT clear the session totals or the order
// sequence: those answer "since this process started", across however many
// loads happen in between -- see OOT_ResetForTest for the test-only reset
// that does clear them.
void OOT_Reset(OotSink sink);

// Test-only: zeros the session totals, the order sequence and the active-time
// clock's running state on top of whatever OOT_Reset(sink) just flushed, so
// each host-test case starts from a clean slate with its own `now` timeline.
// Never called from the game-facing glue.
void OOT_ResetForTest();

// A new player move order for chars[0..n). Any of these characters' current
// membership closes first (end=supersede if that empties its record); an
// open stall there resolves as a user re-order.
void OOT_Begin(const size_t* chars, int n, int cellSpan, double now, bool paused = false);

// The per-poll motion sample for `c` (main-thread poll, every poll whether
// moving or not). `post` is a pure position test against the order's own
// destination, terminal for this member: it retires (arrived, never restarts
// a stall) the first time it reads true, whatever `moving` says.
void OOT_NoteMotion(size_t c, bool moving, bool post, double now, bool paused = false);

// `c` is unconscious or dead: terminal, like arrival. Any open stall is
// dropped, not counted, and the member retires.
void OOT_NoteKo(size_t c, double now, bool paused = false);

// A K7-family re-issue was actually sent for `c`, tagged with its form
// ("arr"/"del"/"park"/"growth"/"retry"). Latches onto an already-open stall,
// and is also kept for OO_K7_SEND_LOOKBACK seconds so a stall that opens just
// after the send still credits it (a fast recovery can resolve before a
// stall ever opens to latch into).
void OOT_NoteReissueSent(size_t c, const char* form, double now, bool paused = false);

// A stop-key, job order or other cancel ended `c`'s participation in its
// current order (not a routing failure): any open stall resolves as a user
// re-order, and `c` retires.
void OOT_Cancel(size_t c, double now, bool paused = false);

// A formation group completed (every alive member within its own approach
// radius, or removed): closes any record shared by chars[0..n) that is not
// already closed by member-level retirement. Every member the group still
// has in this record (i.e. not already retired as KO or arrived) is credited
// as arrived -- the group's own completion is itself the arrival signal for
// whoever is still standing when it fires.
void OOT_OnGroupComplete(const size_t* chars, int n, double now, bool paused = false);

// Per-frame housekeeping: drops a member no longer in `live[0..liveCount)`
// (skipped entirely when `haveList` is false -- an unreadable squad list
// drops nobody), and closes a record whose age passes the safety timeout or
// whose members are all gone.
void OOT_Poll(const size_t* live, int liveCount, bool haveList, double now, bool paused = false);

// A K7 signature or edge-span guess for `c`'s current stall (PLAYER STUCK's
// own k7=/span@arr= fields), latched once per stop and only when the caller
// has already confirmed the stall is not excluded (ko/post).
void OOT_NoteStopGuess(size_t c, const char* guess, double now);

// The route planner owned `c`'s wait this poll: latches onto `c`'s open stall (none open, no-op).
// Such a stall resolves into its record's plannerWait count and nowhere else: not a stop, not a
// recovery class, not stall time.
void OOT_NotePlannerWait(size_t c, double now, bool paused = false);

// Whether the closed-record lines carry the planner column (" plannerWait=<n>"); off by default.
void OOT_SetPlannerColumn(bool on);

struct OotTotals
{
	long orders;       // every order ever begun
	long longOrders;   // cells>=9 orders that have closed
	long longStop;      // ...of those, with at least one qualifying stop
	long longFail;      // ...of those, with a userRec or unrec character
	long userRec;       // character-stall count, cells>=9 orders only
	long unrec;         // character-stall count, cells>=9 orders only
};
OotTotals OOT_GetTotals();

// Open (not yet closed) records, for tests and diagnostics.
int OOT_OpenRecords();

#endif // KENSHI_ZONE_OPT_ORDER_OUTCOME_TABLE_H
