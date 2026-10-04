#include <cstdio>
#include <string>
#include <vector>
#include "movement/order_outcome_table.h"

#include "check.h"
static std::vector<std::string> g_lines;
static void Sink(const std::string& s) { g_lines.push_back(s); }
static bool Has(const std::string& s, const char* tok) { return s.find(tok) != std::string::npos; }
static void Fresh()
{
	OOT_Reset(Sink); g_lines.clear();
	OOT_Reset(Sink); g_lines.clear();
	OOT_ResetForTest();   // OOT_Reset keeps the session totals; zero them here
}

int main()
{
	const size_t A = 0x1000, B = 0x2000, C = 0x3000;
	size_t ab[2] = { A, B };
	size_t live[3] = { A, B, C };

	// 1. Supersede prints exactly once; a 2s+ open stall becomes userRec.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);
	OOT_Begin(&A, 1, 29, 15.0);
	Check(g_lines.size() == 1, "supersede prints the old record once");
	Check(Has(g_lines[0], "userRec=1") && Has(g_lines[0], "unrec=0"), "5s stall then re-order is userRec");

	// 2. Post-arrival wander is not a stop.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, true, true, 90.0);        // passes through the destination radius
	OOT_NoteMotion(A, true, false, 92.0);       // AI job walks it off
	OOT_NoteMotion(A, false, false, 95.0);      // stands 741 off
	OOT_Poll(live, 3, true, 200.0);
	Check(g_lines.size() == 1, "arrival closes a solo record");
	Check(Has(g_lines[0], "arrived=1") && Has(g_lines[0], "unrec=0") && Has(g_lines[0], "userRec=0"),
	      "post-arrival wander is never unrec/userRec");
	Check(OOT_GetTotals().longFail == 0, "wander does not fail the long order");

	// 3. Knockout then wake-up stall is not a stop.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 40.0);
	OOT_NoteKo(A, 41.0);
	OOT_NoteMotion(A, true, false, 100.0);      // wakes, stumbles
	OOT_NoteMotion(A, false, false, 101.0);     // stands with no order
	OOT_Poll(live, 3, true, 700.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], "ko=1") && Has(g_lines[0], "unrec=0"),
	      "KO is terminal for the member; wake-up stall not counted");

	// 4. A member leaving the squad does not close the others; the leader leaving mid-route
	//    (it drops out before the others arrive) keeps the record open.
	Fresh();
	OOT_Begin(ab, 2, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(B, true, false, 1.0);
	size_t onlyB[1] = { B };
	OOT_Poll(onlyB, 1, true, 90.0);
	Check(g_lines.empty() && OOT_OpenRecords() == 1, "A gone, B still walking: record open");
	OOT_NoteMotion(B, true, true, 110.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], "arrived=1"), "B's arrival closes it");

	// 5. Timeout: a stall still open at the safety timeout is unrec, counted once.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 50.0);
	OOT_Poll(live, 3, true, 599.0);
	Check(g_lines.empty(), "no close before the timeout");
	OOT_Poll(live, 3, true, 601.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], "unrec=1") && Has(g_lines[0], "end=timeout"),
	      "open stall at timeout is unrec");
	OOT_Poll(live, 3, true, 700.0);
	Check(g_lines.size() == 1, "a closed record never prints twice");

	// 6. Unreadable squad list: members are not dropped, timeout still closes.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_Poll(NULL, 0, false, 10.0);
	Check(OOT_OpenRecords() == 1, "haveList=false drops nobody");

	// 7. Cancel (stop key / job / attack) is not a failure.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_Cancel(A, 20.0);                        // stopped while walking
	OOT_NoteMotion(A, false, false, 25.0);      // stands after the stop
	OOT_Begin(&A, 1, 29, 60.0);                 // next move order
	Check(Has(g_lines[0], "userRec=0") && Has(g_lines[0], "unrec=0"), "a cancel while walking counts nothing");

	// 8. Table full: the (capacity+1)th concurrent order evicts without
	// inventing unrec, and the evicted record is not counted as a long
	// order outcome either; no growth over 1000 orders.
	Fresh();
	const int CAP = 64;
	for (int i = 0; i < CAP + 1; ++i)
	{
		size_t c = 0x10000 + (size_t)i * 0x100;
		OOT_Begin(&c, 1, 29, (double)i);
		OOT_NoteMotion(c, false, false, (double)i + 0.5);
	}
	Check(g_lines.size() == 1 && Has(g_lines[0], "end=evict") && Has(g_lines[0], "unrec=0"),
	      "eviction is not an outcome");
	Check(OOT_GetTotals().unrec == 0, "eviction adds no unrec to the totals");
	Check(OOT_GetTotals().longOrders == 0, "an evicted record is not a long order outcome");
	Fresh();
	for (int i = 0; i < 1000; ++i)
	{
		size_t c = 0x10000 + (size_t)(i % (CAP + 24)) * 0x100;
		OOT_Begin(&c, 1, 29, (double)i);
	}
	Check(OOT_OpenRecords() <= CAP, "the table never grows past its cap");

	// 8b. A short record is evicted before a long one even when the long one
	// is older: a base-management player's short walk-outs must not starve
	// a long order still genuinely in progress out of its own slot.
	Fresh();
	{
		size_t longChar = 0x20000;
		OOT_Begin(&longChar, 1, 29, 0.0);   // the oldest record, but long
		for (int i = 1; i <= CAP; ++i)
		{
			size_t c = 0x30000 + (size_t)i * 0x100;
			OOT_Begin(&c, 1, 8, (double)i);   // short, fills the rest of the table
		}
	}
	Check(g_lines.size() == 1 && Has(g_lines[0], "cells=8") && Has(g_lines[0], "end=evict"),
	      "a short record is evicted, not the older long one");
	Check(OOT_OpenRecords() == CAP, "the table stays at capacity");
	{
		// The long record is still open: superseding it prints its own
		// line (cells=29), proving it survived the fill rather than having
		// been the one reclaimed.
		size_t longChar = 0x20000;
		OOT_Begin(&longChar, 1, 29, 100.0);
	}
	Check(g_lines.size() == 2 && Has(g_lines[1], "cells=29") && Has(g_lines[1], "end=supersede"),
	      "the long record survived the fill");

	// 9. Group complete closes; long-only totals and the long-order denominator.
	Fresh();
	OOT_Begin(ab, 2, 8, 0.0);                  // short
	OOT_OnGroupComplete(ab, 2, 50.0);
	OOT_Begin(ab, 2, 29, 60.0);                // long, one member user-recovered
	OOT_NoteMotion(A, true, false, 61.0);
	OOT_NoteMotion(A, false, false, 100.0);
	OOT_Begin(&A, 1, 4, 110.0);
	OOT_OnGroupComplete(onlyB, 1, 150.0);
	OotTotals t = OOT_GetTotals();
	Check(t.orders == 3 && t.longOrders == 1, "orders counts all, longOrders counts cells>=9 only");
	Check(t.longFail == 1 && t.userRec == 1, "one failed long order, one user-recovered character");

	// 10. K7 credit for a non-representative group member (formation path).
	Fresh();
	OOT_Begin(ab, 2, 29, 0.0);
	OOT_NoteMotion(B, true, false, 1.0);
	OOT_NoteMotion(B, false, false, 50.0);
	OOT_NoteReissueSent(B, "park", 53.0);
	OOT_NoteMotion(B, true, false, 54.0);
	OOT_NoteMotion(A, true, true, 90.0);
	OOT_NoteMotion(B, true, true, 91.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], "k7rec=1") && Has(g_lines[0], "selfRec=0"),
	      "a group member re-issued via the formation path is k7rec");

	// 11. Self-resolved en route vs start delay -- the glue's real shape: the
	// click poll's spurious "moving" report (StorePlayerClickDest zeroes
	// prevPos, so the very first real poll after a click always reads as a
	// jump) is suppressed to moving=false there, so a genuine path-queue or
	// gather wait can still open before departure is recorded.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, false, false, 1.0);       // first poll after the click (glue passes moving=false)
	OOT_NoteMotion(A, false, false, 2.0);       // still waiting for its path / gather
	OOT_NoteMotion(A, true, false, 4.5);        // departs after a 3.5 s wait: a start delay, not a stop
	OOT_NoteMotion(A, false, false, 40.0);      // a real stop, after departure
	OOT_NoteMotion(A, true, false, 44.0);       // resumes with no re-issue
	OOT_NoteMotion(A, true, true, 90.0);
	Check(Has(g_lines[0], "selfRec=1") && Has(g_lines[0], "walked=1") && OOT_GetTotals().longStop == 1,
	      "the path-queue wait is not counted; the later en-route stop is selfRec");

	// 11b. A start delay with no later stop is never counted at all: this
	// path-queue wait alone must not read as a stop.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, false, false, 1.0);       // first poll after the click
	OOT_NoteMotion(A, false, false, 2.0);
	OOT_NoteMotion(A, true, false, 4.5);        // departs after a 3.5 s path-queue wait
	OOT_NoteMotion(A, true, true, 90.0);
	Check(Has(g_lines[0], "selfRec=0") && OOT_GetTotals().longStop == 0, "path-queue wait alone is a start delay");

	// 12. Reset flushes an open stuck order (reload to escape) and folds it
	// into the session totals -- a reload used to escape a stuck squad
	// must reach longFail, and the totals/order sequence must survive the
	// reset itself (they answer "since this process started", not "since
	// the last load").
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, false, false, 1.0);
	OOT_NoteMotion(A, true, false, 2.0);
	OOT_NoteMotion(A, false, false, 30.0);
	OOT_Reset(Sink);
	Check(g_lines.size() == 1 && Has(g_lines[0], "end=reset") && Has(g_lines[0], "unrec=1"),
	      "a reload while stuck is recorded");
	{
		OotTotals t = OOT_GetTotals();
		Check(t.longOrders == 1 && t.longFail == 1 && t.unrec == 1,
		      "a reload while stuck is a failed long order");
	}
	OOT_Begin(&B, 1, 29, 40.0);
	Check(OOT_GetTotals().orders == 2, "totals and the order sequence survive a reset");

	// 13. A K7 send that lands before the table's own poll ever opens a stall
	// still credits the recovery -- the common arrival re-issue shape, where the send
	// resolves the stop faster than the 1-2s-late stall-open poll can catch
	// it. The stall opens after the send but inside the 2s lookback. The
	// guess is tagged only once PLAYER STUCK's own stop-guess call actually
	// latches: the send's form is remembered, not printed, at open time.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 9.0);        // still moving before the real stop
	OOT_NoteReissueSent(A, "arr", 10.3);        // the arrival re-issue fires ~0.3s after the real stop
	OOT_NoteMotion(A, false, false, 10.9);      // the table's own poll opens the stall late (0.6s after the send)
	OOT_NoteStopGuess(A, "k7=trk/reached/span1", 11.9);  // PLAYER STUCK's own guess, overridden by the send's form
	OOT_NoteMotion(A, true, false, 13.0);       // resumes after a qualifying 2.1s stall
	OOT_NoteMotion(A, true, true, 14.0);        // arrives, closes the record
	Check(g_lines.size() == 1 && Has(g_lines[0], "k7rec=1") && Has(g_lines[0], "selfRec=0"),
	      "a send inside the lookback credits k7rec even though the stall opened after it");
	Check(Has(g_lines[0], "stops=k7=arr"), "the guess is tagged with the send's form");

	// A send older than the 2s lookback when the stall opens does not credit
	// (it is unrelated to this stop).
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteReissueSent(A, "arr", 5.0);         // an earlier, unrelated send
	OOT_NoteMotion(A, false, false, 10.0);      // a later, unrelated stop (>2s after the send)
	OOT_NoteMotion(A, true, false, 13.0);       // resumes after a qualifying 3s stall
	OOT_NoteMotion(A, true, true, 14.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], "selfRec=1") && Has(g_lines[0], "k7rec=0"),
	      "a send older than the lookback does not credit k7rec");

	// 13c. A send-credited stall that never
	// reaches the 2s floor must not tag stops= at all -- the credit is not
	// yet known to be real when the stall opens, so tagging it there would
	// print a K7 form on an order that never actually counted a stop.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 9.0);
	OOT_NoteReissueSent(A, "arr", 10.3);
	OOT_NoteMotion(A, false, false, 10.9);      // stall opens, would-be credit remembered
	OOT_NoteMotion(A, true, false, 11.9);       // resumes after only 1.0s -- never counted
	OOT_NoteMotion(A, true, true, 13.0);        // arrives, closes the record
	Check(g_lines.size() == 1 && Has(g_lines[0], "k7rec=0") && Has(g_lines[0], "selfRec=0"),
	      "a 1s stall is never counted, credited or not");
	Check(Has(g_lines[0], "stops=-"), "an uncounted stall leaves no guess tag");

	// 14. Group completion credits every member it still has as arrived,
	// unless that member was already retired (KO or its own arrival test).
	// A 6-member group where 5 are knocked out and 1 finishes through the
	// group's own approach-radius check (never through OOT_NoteMotion's
	// post=true, the shape a formation completion can beat it to).
	Fresh();
	{
		size_t six[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
		OOT_Begin(six, 6, 29, 0.0);
		OOT_NoteKo(six[1], 5.0);
		OOT_NoteKo(six[2], 5.0);
		OOT_NoteKo(six[3], 5.0);
		OOT_NoteKo(six[4], 5.0);
		OOT_NoteKo(six[5], 5.0);
		OOT_OnGroupComplete(six, 6, 10.0);   // "1 alive, 6/6 done"
		Check(g_lines.size() == 1 && Has(g_lines[0], "ko=5") && Has(g_lines[0], "arrived=1"),
		      "the one alive member is credited as arrived on group completion");
	}

	// A member already retired (post= true through OOT_NoteMotion) before the
	// group completes is not double-counted.
	Fresh();
	OOT_Begin(ab, 2, 29, 0.0);
	OOT_NoteMotion(A, true, true, 5.0);      // A arrives on its own first
	OOT_OnGroupComplete(ab, 2, 10.0);        // only B is still in the record
	Check(g_lines.size() == 1 && Has(g_lines[0], "arrived=2"),
	      "a member already arrived is not double-counted, but B still is");

	// -------------------------------------------------------------------
	// 15-16. The pause gate (order_outcome_table.cpp's ActiveTime clock,
	// paused=true default false so every test above is unaffected).
	// -------------------------------------------------------------------

	// 15. An 11s escape-menu pause mid-walk scores nothing, even though the
	// character reads "not moving" throughout it (the game is not
	// simulating, so that reading proves nothing about a real stall).
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);     // walking
	OOT_NoteMotion(A, false, false, 2.0, true);     // pause begins
	OOT_NoteMotion(A, false, false, 13.0, true);    // still paused, 11s later
	OOT_NoteMotion(A, true, false, 13.5, false);    // resumes walking after unpause
	OOT_NoteMotion(A, true, true, 14.0, false);     // arrives
	Check(g_lines.size() == 1 && Has(g_lines[0], "unrec=0") && Has(g_lines[0], "userRec=0")
	      && Has(g_lines[0], "selfRec=0") && Has(g_lines[0], "stallCharS=0.0"),
	      "an 11s pause mid-walk scores nothing");

	// 16. A real stall that straddles a pause counts only its unpaused time:
	// 2s unpaused, an 11s pause (excluded), then another 1.5s unpaused before
	// motion resumes -- 3.5s counted, not the 14.5s of raw wall time between
	// the stop and the resume.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);     // walking
	OOT_NoteMotion(A, false, false, 4.0, false);    // real stall opens
	OOT_NoteMotion(A, false, false, 6.0, false);    // 2s of real (unpaused) stall so far
	OOT_NoteMotion(A, false, false, 7.0, true);     // pause begins
	OOT_NoteMotion(A, false, false, 18.0, true);    // still paused, 11s later
	OOT_NoteMotion(A, false, false, 18.5, false);   // unpaused, still stopped
	OOT_NoteMotion(A, true, false, 19.5, false);    // resumes moving: stall resolves
	OOT_NoteMotion(A, true, true, 20.0, false);     // arrives, closes the record
	Check(g_lines.size() == 1 && Has(g_lines[0], "selfRec=1") && Has(g_lines[0], "unrec=0")
	      && Has(g_lines[0], "stallCharS=3.5") && Has(g_lines[0], "maxStallS=3.5"),
	      "a stall straddling an 11s pause counts only its 3.5s of unpaused time");

	// 17. The 600s safety timeout excludes paused time.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);
	OOT_Poll(live, 3, true, 2.0, true);
	OOT_Poll(live, 3, true, 700.0, true);          // 698s paused
	OOT_Poll(live, 3, true, 701.0, false);
	Check(g_lines.empty(), "a long pause does not trip the 600s timeout");
	OOT_Poll(live, 3, true, 1300.0, false);        // about 602s active
	Check(g_lines.size() == 1 && Has(g_lines[0], "end=timeout"), "the timeout still fires on active time");

	// 18. The 2s send-lookback measures active time across a pause.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);
	OOT_NoteReissueSent(A, "arr", 5.0, false);
	OOT_Poll(live, 3, true, 5.1, true);
	OOT_Poll(live, 3, true, 20.0, true);           // 14.9s paused
	OOT_NoteMotion(A, false, false, 20.5, false);  // stall opens 0.6s active after the send
	OOT_NoteMotion(A, false, false, 23.0, false);
	OOT_NoteMotion(A, true, false, 23.5, false);   // 3s stall resolves
	OOT_NoteMotion(A, true, true, 24.0, false);
	Check(g_lines.size() == 1 && Has(g_lines[0], "k7rec=1"), "the lookback credit survives a pause");

	// 19. An order issued while paused: the paused start is a start delay,
	// never a stall.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0, true);
	OOT_NoteMotion(A, false, false, 0.5, true);
	OOT_NoteMotion(A, false, false, 30.0, true);
	OOT_NoteMotion(A, true, false, 30.5, false);
	OOT_NoteMotion(A, true, true, 40.0, false);
	Check(g_lines.size() == 1 && Has(g_lines[0], "selfRec=0") && Has(g_lines[0], "stallCharS=0.0"),
	      "a paused start scores nothing");

	// 20. The raw clock stepping backward does not add paused time twice.
	// A(4.0) opens a stall, an ElapsedSec()-style call (OOT_Cancel(B, 4.5))
	// runs ahead of the frame clock, then the frame clock's own next call
	// (4.0) is earlier raw time than that -- ActiveTime must not treat the
	// re-covered [4.0, 4.5) span as paused a second time.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);
	OOT_NoteMotion(A, false, false, 4.0, false);   // stall opens
	OOT_Cancel(B, 4.5, true);                      // an ElapsedSec()-style call, later raw (B not in a record)
	OOT_NoteMotion(A, false, false, 4.0, true);    // frame `now`, earlier raw
	OOT_NoteMotion(A, false, false, 5.0, true);    // real paused span 4.0..5.0 = 1.0s
	OOT_NoteMotion(A, true, false, 9.0, false);    // stall resolves: 5s wall - 1s paused = 4.0
	OOT_NoteMotion(A, true, true, 9.5, false);
	Check(g_lines.size() == 1 && Has(g_lines[0], "stallCharS=4.0"), "no double-counted paused overlap");

	// 21. A save load reached through the pause menu while the squad walks:
	// the paused polls read "not moving" but open no stall, so the reset
	// flushes the order with nothing unrecovered.
	Fresh();
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0, false);    // walking
	OOT_NoteMotion(A, false, false, 2.0, true);    // pause menu open
	OOT_NoteMotion(A, false, false, 3.0, true);
	OOT_Reset(Sink);                               // the load
	Check(g_lines.size() == 1 && Has(g_lines[0], "end=reset") && Has(g_lines[0], "unrec=0")
	      && Has(g_lines[0], "stallCharS=0.0"),
	      "a paused walk flushed by a save load scores nothing");

	// 22. A qualifying stall the route planner owned (latched while it was open) is counted in
	// the planner column only: no stop, no stall time, no recovery class, even with a K7 send
	// inside it that would otherwise credit k7rec.
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);         // stall opens
	OOT_NotePlannerWait(A, 10.5);
	OOT_NoteReissueSent(A, "arr", 12.0);
	OOT_NoteMotion(A, true, false, 16.0);          // 6s stall resolves on motion
	OOT_NoteMotion(A, true, true, 20.0);           // arrives
	{
		OotTotals t = OOT_GetTotals();
		Check(g_lines.size() == 1 && Has(g_lines[0], " plannerWait=1") && Has(g_lines[0], "k7rec=0")
		      && Has(g_lines[0], "userRec=0") && Has(g_lines[0], "unrec=0") && Has(g_lines[0], "selfRec=0")
		      && Has(g_lines[0], "stallCharS=0.0") && Has(g_lines[0], "maxStallS=0.0")
		      && t.longOrders == 1 && t.longStop == 0 && t.longFail == 0 && t.userRec == 0 && t.unrec == 0,
		      "outcome: a planner wait is not a stop");
	}

	// 23. The column: absent while off, present (with its zero) while on, on both print sites.
	Fresh();
	OOT_SetPlannerColumn(false);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, true, 5.0);
	OOT_Begin(&B, 1, 29, 6.0);
	OOT_Reset(Sink);
	bool offClean = g_lines.size() == 2 && !Has(g_lines[0], "plannerWait=")
	                && Has(g_lines[1], "end=reset") && !Has(g_lines[1], "plannerWait=");
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, true, 5.0);
	OOT_Begin(&B, 1, 29, 6.0);
	OOT_Reset(Sink);
	Check(offClean && g_lines.size() == 2 && Has(g_lines[0], " plannerWait=0")
	      && Has(g_lines[1], "end=reset") && Has(g_lines[1], " plannerWait=0"),
	      "outcome: the plannerWait column prints only when set");

	// 24. A planner-wait note with no stall open latches nothing: the stall that opens later is
	// classified as before (k7rec, a stop, its stall time).
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NotePlannerWait(A, 2.0);                   // moving: no stall to latch
	OOT_NoteMotion(A, false, false, 10.0);
	OOT_NoteReissueSent(A, "arr", 12.0);
	OOT_NoteMotion(A, true, false, 16.0);
	OOT_NoteMotion(A, true, true, 20.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], " plannerWait=0") && Has(g_lines[0], "k7rec=1")
	      && Has(g_lines[0], "stallCharS=6.0") && OOT_GetTotals().longStop == 1,
	      "outcome: an unlatched stall is classified as before");

	// 25. A latch ends with its stall: the member's next, unlatched stall is a stop again.
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);
	OOT_NotePlannerWait(A, 11.0);
	OOT_NoteMotion(A, true, false, 15.0);          // the planner's wait ends
	OOT_NoteMotion(A, false, false, 30.0);         // an ordinary stall
	OOT_NoteMotion(A, true, false, 34.0);
	OOT_NoteMotion(A, true, true, 40.0);
	Check(g_lines.size() == 1 && Has(g_lines[0], " plannerWait=1") && Has(g_lines[0], "selfRec=1")
	      && Has(g_lines[0], "stallCharS=4.0"),
	      "outcome: a planner-wait latch ends with its stall");

	// 26. The planner owns the wait for two polls, then a poll passes unowned while the member is
	// still stopped: the owned segment ends at the next sample, and the stop that goes on is an
	// ordinary stall that the K7 send inside it rescues.
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);         // stall opens
	OOT_NotePlannerWait(A, 10.0);                  // owned
	OOT_NoteMotion(A, false, false, 13.0);
	OOT_NotePlannerWait(A, 13.0);                  // owned
	OOT_NoteMotion(A, false, false, 16.0);         // not owned from here on
	OOT_NoteMotion(A, false, false, 19.0);         // owned segment 10..19 ends; the stop goes on
	OOT_NoteReissueSent(A, "arr", 20.0);
	OOT_NoteMotion(A, false, false, 22.0);
	OOT_NoteMotion(A, true, false, 26.0);          // 7s unowned stall resolves on motion
	OOT_NoteMotion(A, true, true, 30.0);
	{
		OotTotals t = OOT_GetTotals();
		Check(g_lines.size() == 1 && Has(g_lines[0], " plannerWait=1") && Has(g_lines[0], "k7rec=1")
		      && Has(g_lines[0], "selfRec=0") && Has(g_lines[0], "unrec=0") && Has(g_lines[0], "stallCharS=7.0")
		      && t.longStop == 1 && t.longFail == 0,
		      "outcome: a stall that outlives the planner's ownership is credited to its real rescue");
	}

	// 27. A member already stopped past the threshold gains an owned wait: the unowned segment is
	// a stop with no recovery class, and the owned remainder is the planner's wait.
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);         // stall opens
	OOT_NoteMotion(A, false, false, 13.0);
	OOT_NoteMotion(A, false, false, 16.0);
	OOT_NotePlannerWait(A, 16.0);                  // owned: segment 10..16 is a stop
	OOT_NoteMotion(A, false, false, 19.0);
	OOT_NotePlannerWait(A, 19.0);
	OOT_NoteMotion(A, true, false, 22.0);          // the 16..22 owned wait resolves on motion
	OOT_NoteMotion(A, true, true, 25.0);
	{
		OotTotals t = OOT_GetTotals();
		Check(g_lines.size() == 1 && Has(g_lines[0], " plannerWait=1") && Has(g_lines[0], "selfRec=0")
		      && Has(g_lines[0], "k7rec=0") && Has(g_lines[0], "userRec=0") && Has(g_lines[0], "unrec=0")
		      && Has(g_lines[0], "stallCharS=6.0") && Has(g_lines[0], "maxStallS=6.0")
		      && t.longStop == 1 && t.longFail == 0,
		      "outcome: a stuck member that gains an owned wait splits its stall");
	}

	// 28. A save load flushes an open owned wait into plannerWait, not unrec.
	Fresh();
	OOT_SetPlannerColumn(true);
	OOT_Begin(&A, 1, 29, 0.0);
	OOT_NoteMotion(A, true, false, 1.0);
	OOT_NoteMotion(A, false, false, 10.0);
	OOT_NotePlannerWait(A, 11.0);
	OOT_Reset(Sink);
	Check(g_lines.size() == 1 && Has(g_lines[0], "unrec=0") && Has(g_lines[0], " plannerWait=1")
	      && OOT_GetTotals().longFail == 0,
	      "outcome: a planner wait open at a reset is not unrec");
	OOT_SetPlannerColumn(false);

	// The coordinates and the trace's queries: the line carries the order's coordinates, the open record
	// answers its number and its coordinates, and the close note fires once, at the close.
	{
		Fresh();
		static int s_notes = 0, s_noted = 0;
		struct NoteSink { static void Note(int n) { ++s_notes; s_noted = n; } };
		OOT_SetCloseNote(&NoteSink::Note);
		const float from[2] = { 10.0f, 20.0f }, to[2] = { 30.0f, 40.0f };
		OOT_Begin(&A, 1, 29, 0.0, false, from, to);
		int num = OOT_OrderOf(A);
		float xy[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Check(num > 0 && OOT_OrderOf(B) == 0 && OOT_OrderCoords(num, xy) && xy[0] == 10.0f && xy[3] == 40.0f,
		      "trace query: the open record answers its number and coordinates");
		OOT_Cancel(A, 5.0);
		Check(g_lines.size() == 1 && Has(g_lines[0], " cells=29 from=(10,20) to=(30,40) members=1")
		      && s_notes == 1 && s_noted == num && OOT_OrderOf(A) == 0,
		      "coords: the closed line carries them and the close note fires once");
		OOT_Begin(&B, 1, 29, 6.0);
		OOT_Cancel(B, 7.0);
		Check(g_lines.size() == 2 && Has(g_lines[1], " from=- to=-"), "coords: a record begun without them reads from=- to=-");
		OOT_SetCloseNote(NULL);
	}

	return CheckExit("order_outcome_table_units");
}
