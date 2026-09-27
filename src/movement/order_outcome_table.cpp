// order_outcome_table.cpp -- the player order-outcome record table, pure
// (see order_outcome_table.h). No game headers, no threading of its own: the
// glue (order_outcome.cpp) guarantees every call is main-thread.

#include "movement/order_outcome_table.h"
#include "movement/order_outcome_policy.h"
#include <sstream>

namespace order_outcome_table_detail {

const int MAX_ORDER_RECORDS  = 64;
const int MAX_ORDER_MEMBERS  = 64;
const double ORDER_TIMEOUT   = 600.0;   // a backstop, not the working mechanism

struct OOMember
{
	size_t character;
	bool   inOrder;
	bool   departed;        // has moved at least once (walked= counts this)
	bool   koLatched;       // rec.ko already counts this character
	bool   arrivedLatched;  // rec.arrived already counts this character
	double stallStart;      // 0.0 = not currently stalled
	bool   stallExcluded;   // ko or post ends this stall: never counted
	bool   stallK7Sent;     // a K7-family re-issue landed during this stall
	double lastK7Send;      // 0.0 = none; last OOT_NoteReissueSent, whether or not a stall is open
	const char* lastK7SendForm; // its form ("arr"/"del"/...), owned by the caller (a string literal)
	const char* stallK7Form;    // the send's form once it credited the *open* stall (until
	                            // OOT_NoteStopGuess actually tags the line with it)
};

struct OrderRecord
{
	bool        active;
	int         orderNum;
	double      issueTime;
	int         cellSpan;
	int         memberCount;
	OOMember    members[MAX_ORDER_MEMBERS];

	int         arrived, ko, k7rec, userRec, unrec, selfRec;
	int         stopCount;      // qualifying, non-excluded stalls (any resolution)
	double      stallCharS;
	double      maxStallS;
	std::string stopsGuess;
};

OrderRecord g_records[MAX_ORDER_RECORDS];
int         g_nextOrderNum = 0;
OotSink     g_sink = NULL;

long g_totalOrders     = 0;
long g_totalLongOrders = 0;
long g_totalLongStop   = 0;
long g_totalLongFail   = 0;
long g_totalUserRec    = 0;
long g_totalUnrec      = 0;

// The active-time clock: `now` minus every second spent paused so far. A
// single running total shared by every OOT_* call, rather than a per-call
// paused check, so a stall that opens before a pause and resolves after it
// still measures only its unpaused duration. Calls must arrive in
// non-decreasing raw-time order (the caller's own frame clock already
// guarantees this); a call from before the last-seen `now` contributes
// nothing rather than running the clock backward.
double g_activeClockLastRaw = -1.0;
double g_activeClockPausedAccum = 0.0;

double ActiveTime(double rawNow, bool paused)
{
	if (g_activeClockLastRaw < 0.0) g_activeClockLastRaw = rawNow;
	double dt = rawNow - g_activeClockLastRaw;
	// Only a forward step advances the clock's own bookkeeping. The glue
	// mixes two raw-time sources (the frame `now` and ElapsedSec()-stamped
	// order/cancel/group-complete calls), so a later-captured raw time can be
	// followed by an earlier one; advancing g_activeClockLastRaw backward
	// there would double-count the overlap into the paused total on the next
	// call. A `dt <= 0` call contributes nothing and leaves the clock where
	// it was.
	if (dt > 0.0)
	{
		if (paused) g_activeClockPausedAccum += dt;
		g_activeClockLastRaw = rawNow;
	}
	return rawNow - g_activeClockPausedAccum;
}

OOMember* FindMember(size_t character, OrderRecord** outRec)
{
	if (!character) return NULL;
	for (int r = 0; r < MAX_ORDER_RECORDS; ++r)
	{
		if (!g_records[r].active) continue;
		OrderRecord& rec = g_records[r];
		for (int m = 0; m < rec.memberCount; ++m)
		{
			if (rec.members[m].inOrder && rec.members[m].character == character)
			{
				if (outRec) *outRec = &rec;
				return &rec.members[m];
			}
		}
	}
	return NULL;
}

bool AnyMemberLeft(const OrderRecord& rec)
{
	for (int m = 0; m < rec.memberCount; ++m)
		if (rec.members[m].inOrder) return true;
	return false;
}

// Resolves member m's stall (if any) with resolution `how` and folds the
// result into rec's aggregates. A no-op when m is not currently stalled.
void ResolveStall(OrderRecord& rec, OOMember& m, OrderOutcomeResolution how, double now)
{
	if (m.stallStart <= 0.0) return;
	double dur = now - m.stallStart;
	bool qualifies = OrderOutcomeStallQualifies(dur);
	OrderOutcomeRecovery kind = OrderOutcomeClassifyStop(qualifies, m.stallExcluded, how, m.stallK7Sent);

	if (qualifies && !m.stallExcluded)
	{
		rec.stopCount++;
		rec.stallCharS += dur;
		if (dur > rec.maxStallS) rec.maxStallS = dur;
	}
	switch (kind)
	{
	case OO_REC_SELF:  rec.selfRec++;  break;
	case OO_REC_K7:    rec.k7rec++;    break;
	case OO_REC_USER:  rec.userRec++;  break;
	case OO_REC_UNREC: rec.unrec++;    break;
	default: break;
	}

	m.stallStart    = 0.0;
	m.stallExcluded = false;
	m.stallK7Sent   = false;
	m.stallK7Form   = NULL;
}

// Eviction is a bookkeeping event, never a routing answer -- an evicted
// long order in progress must not add to longOrders (its open stall was
// just dropped, not resolved), or longFail/longOrders would be biased
// toward success by every eviction. Shared by PrintAndClose and OOT_Reset's
// flush, so a reset counts a flushed record exactly the way a normal close
// would.
void FoldIntoTotals(const OrderRecord& rec, OrderOutcomeResolution finalRes)
{
	if (finalRes == OO_RESOLVE_EVICT) return;
	if (!OrderOutcomeIsLong(rec.cellSpan)) return;
	g_totalLongOrders++;
	if (rec.stopCount > 0) g_totalLongStop++;
	if (rec.userRec + rec.unrec > 0) g_totalLongFail++;
	g_totalUserRec += rec.userRec;
	g_totalUnrec   += rec.unrec;
}

void PrintAndClose(OrderRecord& rec, double now, const char* endReason,
                    OrderOutcomeResolution finalRes = OO_RESOLVE_CLOSE)
{
	if (!rec.active) return;
	for (int m = 0; m < rec.memberCount; ++m)
		ResolveStall(rec, rec.members[m], finalRes, now);

	int walked = 0;
	for (int m = 0; m < rec.memberCount; ++m)
		if (rec.members[m].departed) walked++;

	if (g_sink)
		g_sink(OrderOutcomeFormatLine(rec.orderNum, rec.issueTime, rec.cellSpan,
		                               rec.memberCount, walked, rec.arrived, rec.ko,
		                               rec.k7rec, rec.userRec, rec.unrec, rec.selfRec,
		                               rec.stallCharS, rec.maxStallS, rec.stopsGuess, endReason));

	FoldIntoTotals(rec, finalRes);
	rec.active = false;
}

// Arrival and knockout are terminal for a member: whatever it was doing
// ends, cleanly, and it never re-opens a stall of its own. Shared by the
// post= and ko= paths below -- the caller has already bumped the matching
// latch counter (rec.arrived / rec.ko) before calling this.
void RetireMember(OrderRecord& rec, OOMember& m, double now)
{
	m.stallExcluded = true;
	ResolveStall(rec, m, OO_RESOLVE_CLOSE, now);   // excluded forces NONE regardless of `how`
	m.inOrder = false;
	if (!AnyMemberLeft(rec))
		PrintAndClose(rec, now, "done");
}

} // namespace
using namespace order_outcome_table_detail;

// A save-load reset flushes every open record (a reload used to escape a
// stuck squad must reach the totals like any other close). It does NOT zero the
// session totals or the order sequence: those are meant to answer "since
// this process started", across however many loads happen in between, not
// "since the last load". OOT_ResetForTest is the test-only variant that
// also zeros them, for a clean slate between host-test cases.
void OOT_Reset(OotSink sink)
{
	g_sink = sink;
	for (int r = 0; r < MAX_ORDER_RECORDS; ++r)
	{
		OrderRecord& rec = g_records[r];
		if (!rec.active) continue;
		// No reliable "now" across a reload: an open stall is a genuine
		// unrecovered failure, counted without a measured duration (so it
		// adds to unrec but not to stallCharS/maxStallS).
		for (int m = 0; m < rec.memberCount; ++m)
		{
			OOMember& mem = rec.members[m];
			if (mem.stallStart > 0.0 && !mem.stallExcluded)
				rec.unrec++;
			mem.stallStart = 0.0;
		}
		int walked = 0;
		for (int m = 0; m < rec.memberCount; ++m)
			if (rec.members[m].departed) walked++;
		if (g_sink)
			g_sink(OrderOutcomeFormatLine(rec.orderNum, rec.issueTime, rec.cellSpan,
			                               rec.memberCount, walked, rec.arrived, rec.ko,
			                               rec.k7rec, rec.userRec, rec.unrec, rec.selfRec,
			                               rec.stallCharS, rec.maxStallS, rec.stopsGuess, "reset"));
		FoldIntoTotals(rec, OO_RESOLVE_CLOSE);
		rec.active = false;
	}
}

void OOT_ResetForTest()
{
	g_totalOrders = g_totalLongOrders = g_totalLongStop = g_totalLongFail = 0;
	g_totalUserRec = g_totalUnrec = 0;
	g_nextOrderNum = 0;
	g_activeClockLastRaw = -1.0;
	g_activeClockPausedAccum = 0.0;
}

void OOT_Begin(const size_t* chars, int n, int cellSpan, double now, bool paused)
{
	now = ActiveTime(now, paused);
	// A new order for a character already in a record closes that record's
	// membership for it first: an open stall there is exactly a user
	// re-order.
	for (int i = 0; i < n; ++i)
	{
		OrderRecord* rec; OOMember* m = FindMember(chars[i], &rec);
		if (!m) continue;
		ResolveStall(*rec, *m, OO_RESOLVE_SUPERSEDE, now);
		m->inOrder = false;
		if (!AnyMemberLeft(*rec))
			PrintAndClose(*rec, now, "supersede");
	}

	if (n <= 0 || !chars) return;

	int slot = -1;
	// Eviction candidates, tracked separately: a short (cells<9) record is
	// evicted before any long one, so a base-management player's short
	// walk-outs don't starve a long order still genuinely in progress out of
	// its own record. Oldest first within each tier.
	double oldestShort = 1.0e300; int oldestShortSlot = -1;
	double oldestAny   = 1.0e300; int oldestAnySlot   = 0;
	for (int r = 0; r < MAX_ORDER_RECORDS; ++r)
	{
		if (!g_records[r].active) { slot = r; break; }
		if (g_records[r].issueTime < oldestAny)
		{
			oldestAny = g_records[r].issueTime; oldestAnySlot = r;
		}
		if (!OrderOutcomeIsLong(g_records[r].cellSpan) && g_records[r].issueTime < oldestShort)
		{
			oldestShort = g_records[r].issueTime; oldestShortSlot = r;
		}
	}
	if (slot < 0)
	{
		// Every slot busy: reclaim a short record if one exists, else the
		// oldest overall. Eviction is bookkeeping, not an outcome --
		// OO_RESOLVE_EVICT keeps any open stall out of every bucket, and
		// FoldIntoTotals keeps it out of longOrders/longFail too.
		int evictSlot = (oldestShortSlot >= 0) ? oldestShortSlot : oldestAnySlot;
		PrintAndClose(g_records[evictSlot], now, "evict", OO_RESOLVE_EVICT);
		slot = evictSlot;
	}

	OrderRecord& rec = g_records[slot];
	rec.active      = true;
	rec.orderNum    = ++g_nextOrderNum;
	rec.issueTime   = now;
	rec.cellSpan    = cellSpan;
	rec.memberCount = (n > MAX_ORDER_MEMBERS) ? MAX_ORDER_MEMBERS : n;
	for (int i = 0; i < rec.memberCount; ++i)
	{
		OOMember& m = rec.members[i];
		m.character      = chars[i];
		m.inOrder        = true;
		m.departed       = false;
		m.koLatched      = false;
		m.arrivedLatched = false;
		m.stallStart     = 0.0;
		m.stallExcluded  = false;
		m.stallK7Sent    = false;
		m.stallK7Form    = NULL;
		m.lastK7Send     = 0.0;
		m.lastK7SendForm = NULL;
	}
	rec.arrived = rec.ko = rec.k7rec = rec.userRec = rec.unrec = rec.selfRec = 0;
	rec.stopCount   = 0;
	rec.stallCharS  = 0.0;
	rec.maxStallS   = 0.0;
	rec.stopsGuess.clear();

	g_totalOrders++;
}

void OOT_NoteMotion(size_t c, bool moving, bool post, double now, bool paused)
{
	now = ActiveTime(now, paused);
	OrderRecord* rec; OOMember* m = FindMember(c, &rec);
	if (!m) return;

	if (post)
	{
		if (!m->arrivedLatched) { m->arrivedLatched = true; rec->arrived++; }
		RetireMember(*rec, *m, now);
		return;
	}

	if (moving)
	{
		bool wasDeparted = m->departed;
		m->departed = true;
		if (m->stallStart > 0.0)
		{
			if (!wasDeparted)
			{
				// A start delay (queued behind the path thread, or waiting
				// for its group to gather): the character has not failed to
				// go anywhere, it has not gone anywhere yet. Not counted,
				// whatever its duration.
				m->stallStart    = 0.0;
				m->stallExcluded = false;
				m->stallK7Sent   = false;
				m->stallK7Form   = NULL;
			}
			else
			{
				ResolveStall(*rec, *m, OO_RESOLVE_MOTION, now);
			}
		}
	}
	else if (m->stallStart <= 0.0 && !paused)
	{
		m->stallStart = now;
		// A K7 send that landed just before this stall opened would otherwise
		// never be latched: a stall only opens 1-2s after the real stop, and
		// a fast recovery can resolve inside that gap. Remember the form for
		// OOT_NoteStopGuess to tag the line with -- not here, since this
		// stall has not yet reached the 2s floor and may never be counted at
		// all. Clearing lastK7Send means the same send cannot also credit a
		// later, unrelated stall.
		if (m->lastK7Send > 0.0 && now - m->lastK7Send <= OO_K7_SEND_LOOKBACK)
		{
			m->stallK7Sent = true;
			m->stallK7Form = m->lastK7SendForm;
			m->lastK7Send  = 0.0;
		}
	}
}

void OOT_NoteKo(size_t c, double now, bool paused)
{
	now = ActiveTime(now, paused);
	OrderRecord* rec; OOMember* m = FindMember(c, &rec);
	if (!m) return;
	if (!m->koLatched) { m->koLatched = true; rec->ko++; }
	RetireMember(*rec, *m, now);
}

void OOT_NoteReissueSent(size_t c, const char* form, double now, bool paused)
{
	now = ActiveTime(now, paused);
	OrderRecord* rec; OOMember* m = FindMember(c, &rec);
	if (!m) return;
	m->lastK7Send     = now;
	m->lastK7SendForm = form;
	if (m->stallStart > 0.0) m->stallK7Sent = true;
}

void OOT_Cancel(size_t c, double now, bool paused)
{
	now = ActiveTime(now, paused);
	OrderRecord* rec; OOMember* m = FindMember(c, &rec);
	if (!m) return;
	ResolveStall(*rec, *m, OO_RESOLVE_SUPERSEDE, now);
	m->inOrder = false;
	if (!AnyMemberLeft(*rec))
		PrintAndClose(*rec, now, "cancel");
}

void OOT_OnGroupComplete(const size_t* chars, int n, double now, bool paused)
{
	now = ActiveTime(now, paused);
	for (int i = 0; i < n; ++i)
	{
		if (!chars[i]) continue;
		OrderRecord* rec; OOMember* m = FindMember(chars[i], &rec);
		if (!m) continue;
		ResolveStall(*rec, *m, OO_RESOLVE_CLOSE, now);
		// The group's own completion (every alive member within its own
		// approach radius) is itself an arrival signal: a member still in
		// this record here has not already been retired as KO or as a
		// separately-detected arrival (FindMember only returns members still
		// `inOrder`), so it is credited now rather than left uncounted.
		if (!m->arrivedLatched) { m->arrivedLatched = true; rec->arrived++; }
		m->inOrder = false;
		if (!AnyMemberLeft(*rec))
			PrintAndClose(*rec, now, "done");
	}
}

void OOT_Poll(const size_t* live, int liveCount, bool haveList, double now, bool paused)
{
	now = ActiveTime(now, paused);
	for (int r = 0; r < MAX_ORDER_RECORDS; ++r)
	{
		OrderRecord& rec = g_records[r];
		if (!rec.active) continue;

		bool timedOut = (now - rec.issueTime) > ORDER_TIMEOUT;
		if (haveList)
		{
			for (int m = 0; m < rec.memberCount; ++m)
			{
				OOMember& mem = rec.members[m];
				if (!mem.inOrder) continue;
				bool isLive = false;
				for (int j = 0; j < liveCount; ++j)
					if (live[j] == mem.character) { isLive = true; break; }
				if (!isLive)
				{
					ResolveStall(rec, mem, OO_RESOLVE_CLOSE, now);
					mem.inOrder = false;
				}
			}
		}
		if (timedOut)
			PrintAndClose(rec, now, "timeout");
		else if (!AnyMemberLeft(rec))
			PrintAndClose(rec, now, "done");
	}
}

void OOT_NoteStopGuess(size_t c, const char* guess, double now)
{
	(void)now;
	OrderRecord* rec; OOMember* m = FindMember(c, &rec);
	if (!m || !guess) return;
	if (m->stallStart > 0.0 && rec->stopsGuess.empty())
	{
		// A credited send's own form wins over the caller's class guess: the
		// stall has reached PLAYER STUCK's own second still poll (the same
		// timing the guess itself latches at), so it is real enough to tag.
		if (m->stallK7Form)
			rec->stopsGuess = std::string("k7=") + m->stallK7Form;
		else
			rec->stopsGuess = guess;
	}
}

OotTotals OOT_GetTotals()
{
	OotTotals t;
	t.orders     = g_totalOrders;
	t.longOrders = g_totalLongOrders;
	t.longStop   = g_totalLongStop;
	t.longFail   = g_totalLongFail;
	t.userRec    = g_totalUserRec;
	t.unrec      = g_totalUnrec;
	return t;
}

int OOT_OpenRecords()
{
	int n = 0;
	for (int r = 0; r < MAX_ORDER_RECORDS; ++r)
		if (g_records[r].active) n++;
	return n;
}
