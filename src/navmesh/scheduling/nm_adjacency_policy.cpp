#include "navmesh/scheduling/nm_adjacency_policy.h"
#include <math.h>
#include <string.h>

static int Abs(int v) { return v < 0 ? -v : v; }

int NmAdjCellOf(float v, float cellSize)
{
	return (int)floorf((cellSize * NMADJ_GRID_HALF_CELLS + v) / cellSize);
}

// A box this far from its zone is not a building; keep the reach bounded.
static const int kMaxReach = 4;

void NmAdjDescribe(int x, int y, int type, bool haveUid, unsigned int uid,
                   const float* box, float cellX, float cellZ, NmJobDesc* out)
{
	out->x = x;
	out->y = y;
	out->type = type;
	if (type == 3)
		out->kind = NMADJ_KIND_I;
	else if (type == 4)
		out->kind = !haveUid ? NMADJ_KIND_UNKNOWN
		          : (uid >= NMADJ_INTERIOR_UID_MIN ? NMADJ_KIND_I : NMADJ_KIND_E);
	else
		out->kind = NMADJ_KIND_E;

	out->rx0 = x - 1; out->rx1 = x + 1;
	out->ry0 = y - 1; out->ry1 = y + 1;
	if (out->kind == NMADJ_KIND_E || !box || !(cellX > 0.0f) || !(cellZ > 0.0f))
		return;
	// stitchInterior visits every cell from the box's minimum to its maximum.
	int ax = NmAdjCellOf(box[0] - box[3], cellX), bx = NmAdjCellOf(box[0] + box[3], cellX);
	int az = NmAdjCellOf(box[2] - box[5], cellZ), bz = NmAdjCellOf(box[2] + box[5], cellZ);
	if (ax > bx || az > bz)
		return;   // NaN or an inverted box: the neighbourhood stands
	if (ax < x - kMaxReach) ax = x - kMaxReach;
	if (bx > x + kMaxReach) bx = x + kMaxReach;
	if (az < y - kMaxReach) az = y - kMaxReach;
	if (bz > y + kMaxReach) bz = y + kMaxReach;
	if (ax < out->rx0) out->rx0 = ax;
	if (bx > out->rx1) out->rx1 = bx;
	if (az < out->ry0) out->ry0 = az;
	if (bz > out->ry1) out->ry1 = bz;
}

bool NmAdjReaches(const NmJobDesc& d, int cx, int cy)
{
	return cx >= d.rx0 && cx <= d.rx1 && cy >= d.ry0 && cy <= d.ry1;
}

bool NmJobsConflict(const NmJobDesc& a, const NmJobDesc& b)
{
	const int dx = Abs(a.x - b.x), dy = Abs(a.y - b.y);
	const bool aE = a.kind == NMADJ_KIND_E, bE = b.kind == NMADJ_KIND_E;
	const bool aU = a.kind == NMADJ_KIND_UNKNOWN, bU = b.kind == NMADJ_KIND_UNKNOWN;

	if (aU || bU)
		return (dx <= 1 && dy <= 1) || NmAdjReaches(a, b.x, b.y) || NmAdjReaches(b, a.x, a.y);
	if (aE && bE)
		return dx + dy <= 1;
	if (aE)
		return NmAdjReaches(b, a.x, a.y);
	if (bE)
		return NmAdjReaches(a, b.x, b.y);
	// Two interiors share nothing unless one stitches a live instance in place
	// (type 4) that a regeneration of the same building would swap. A type-4
	// zone is the cell of the box minimum, one cell at most from the building's.
	return (a.type == 4 || b.type == 4) && dx <= 1 && dy <= 1;
}

// ---------------------------------------------------------------------------

void NmAdjInit(NmAdjRegistry* r)
{
	memset(r, 0, sizeof(*r));
	for (int i = 0; i < NMADJ_CAP; ++i)
		r->e[i].owner = NMADJ_NO_OWNER;
}

int NmAdjFindBlocker(const NmAdjRegistry* r, const NmJobDesc& d, unsigned __int64 task, long ownResSeq)
{
	for (int i = 0; i < r->high; ++i)
	{
		const NmAdjEntry& e = r->e[i];
		if (e.state == NMADJ_FREE)
			continue;
		if (e.state == NMADJ_RESERVED)
		{
			if (e.task == task)
				continue;
			if (ownResSeq != 0 && e.resSeq > ownResSeq)
				continue;   // younger than this job's own reservation
		}
		if (NmJobsConflict(d, e.d))
			return i;
	}
	return -1;
}

int NmAdjAdd(NmAdjRegistry* r, const NmJobDesc& d, unsigned __int64 task, int state, int owner, __int64 now)
{
	for (int i = 0; i < NMADJ_CAP; ++i)
	{
		NmAdjEntry& e = r->e[i];
		if (e.state != NMADJ_FREE)
			continue;
		e.d = d;
		e.task = task;
		e.state = state;
		e.owner = owner;
		e.pubSeq = 0;
		e.resSeq = (state == NMADJ_RESERVED) ? ++r->resSeq : 0;
		e.since = now;
		if (i >= r->high)
			r->high = i + 1;
		++r->live;
		return i;
	}
	return -1;
}

void NmAdjFree(NmAdjRegistry* r, int idx)
{
	if (idx < 0 || idx >= NMADJ_CAP || r->e[idx].state == NMADJ_FREE)
		return;
	if (r->e[idx].state == NMADJ_PUBLISHED)
		--r->published;
	r->e[idx].state = NMADJ_FREE;
	r->e[idx].owner = NMADJ_NO_OWNER;
	r->e[idx].task = 0;
	--r->live;
	while (r->high > 0 && r->e[r->high - 1].state == NMADJ_FREE)
		--r->high;
}

void NmAdjPublish(NmAdjRegistry* r, int idx)
{
	if (idx < 0 || idx >= NMADJ_CAP || r->e[idx].state != NMADJ_CLAIMED)
		return;
	r->e[idx].state = NMADJ_PUBLISHED;
	r->e[idx].owner = NMADJ_NO_OWNER;
	r->e[idx].pubSeq = ++r->pubSeq;
	++r->published;
}

int NmAdjFindOwned(const NmAdjRegistry* r, int owner, int state)
{
	for (int i = 0; i < r->high; ++i)
		if (r->e[i].state == state && r->e[i].owner == owner)
			return i;
	return -1;
}

int NmAdjFindReservationOf(const NmAdjRegistry* r, unsigned __int64 task)
{
	for (int i = 0; i < r->high; ++i)
		if (r->e[i].state == NMADJ_RESERVED && r->e[i].task == task)
			return i;
	return -1;
}

long NmAdjOwnResSeq(const NmAdjRegistry* r, unsigned __int64 task)
{
	long own = 0;
	for (int i = 0; i < r->high; ++i)
		if (r->e[i].state == NMADJ_RESERVED && r->e[i].task == task && (own == 0 || r->e[i].resSeq < own))
			own = r->e[i].resSeq;
	return own;
}

// Frees every reservation `task` holds; the oldest one's `since` goes to
// *since. False when it held none.
static bool FreeReservationsOf(NmAdjRegistry* r, unsigned __int64 task, __int64* since)
{
	bool any = false;
	long oldest = 0;
	for (int i = 0; i < r->high; ++i)
	{
		NmAdjEntry& e = r->e[i];
		if (e.state != NMADJ_RESERVED || e.task != task)
			continue;
		if (!any || e.resSeq < oldest)
		{
			oldest = e.resSeq;
			*since = e.since;
		}
		any = true;
		NmAdjFree(r, i);
	}
	return any;
}

int NmAdjReleaseDrained(NmAdjRegistry* r, long seqAtRead, const unsigned __int64* done, int n)
{
	int freed = 0;
	for (int i = 0; i < r->high; ++i)
	{
		NmAdjEntry& e = r->e[i];
		if (e.state != NMADJ_PUBLISHED || e.pubSeq > seqAtRead)
			continue;
		bool present = false;
		for (int k = 0; k < n && !present; ++k)
			present = (done[k] == e.task);
		if (!present)
		{
			NmAdjFree(r, i);
			++freed;
		}
	}
	return freed;
}

// ---------------------------------------------------------------------------

void NmAdjScanBegin(NmAdjScan* s, const NmAdjRegistry* r, bool enforce, __int64 now)
{
	memset(s, 0, sizeof(*s));
	s->enforce = enforce;
	s->now = now;
	s->ageRes = NmAdjFindOwned(r, NMADJ_AGE, NMADJ_RESERVED);
	s->ageResTask = s->ageRes >= 0 ? r->e[s->ageRes].task : 0;
}

void NmAdjScanObserve(NmAdjScan* s, unsigned __int64 task, bool eligible)
{
	if (s->ageRes >= 0 && task == s->ageResTask && !s->ageResSeen)
	{
		s->ageResSeen = true;
		s->ageResEligible = eligible;
	}
}

NmAdjOffer NmAdjScanOffer(NmAdjScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                          const NmJobDesc* d, bool eligible)
{
	NmAdjScanObserve(s, task, eligible);
	if (!eligible || s->took)
		return NMADJ_OFFER_PASS;

	const bool blocked = NmAdjFindBlocker(r, *d, task, NmAdjOwnResSeq(r, task)) >= 0;
	if (blocked && s->enforce)
	{
		++s->skips;
		if (!s->firstSkipped)
		{
			s->firstSkipped = task;
			s->firstSkippedDesc = *d;
		}
		return NMADJ_OFFER_SKIP;
	}
	s->took = true;
	s->tookConflicting = blocked;
	s->taken = task;
	return NMADJ_OFFER_TAKE;
}

bool NmAdjScanWantsRest(const NmAdjScan* s)
{
	return s->ageRes >= 0 && !s->ageResSeen;
}

void NmAdjScanEnd(NmAdjScan* s, NmAdjRegistry* r, int owner, const NmJobDesc* takenDesc,
                  __int64 ageAfter, bool canReserve, NmAdjScanResult* out)
{
	memset(out, 0, sizeof(*out));
	out->claimIdx = -1;

	if (s->took)
	{
		if (s->taken == r->ageTask)
		{
			out->deferredTaken = true;
			out->deferredFor = s->now - r->ageSince;
			r->ageTask = 0;
		}
		out->claimIdx = NmAdjAdd(r, *takenDesc, s->taken, NMADJ_CLAIMED, owner, s->now);
		if (out->claimIdx < 0)
		{
			out->full = true;
			if (s->enforce)
			{
				out->refused = true;
				s->took = false;
			}
		}
		// The bg thread's reservation of this task, if any, ends with the claim.
		const int bgRes = NmAdjFindOwned(r, NMADJ_BG, NMADJ_RESERVED);
		if (s->took && bgRes >= 0 && r->e[bgRes].task == s->taken)
		{
			const __int64 d = s->now - r->e[bgRes].since;
			if (!out->deferredTaken || d > out->deferredFor)
				out->deferredFor = d;
			out->deferredTaken = true;
			NmAdjFree(r, bgRes);
		}
	}

	// The age reservation lives while its job is still one a worker could take.
	if (s->ageRes >= 0)
	{
		const bool claimed = s->took && s->taken == s->ageResTask;
		const bool stale = !s->ageResSeen || !s->ageResEligible;
		if (claimed || stale)
		{
			NmAdjFree(r, s->ageRes);
			out->droppedAge = !claimed;
		}
	}

	if (!s->enforce)
		return;
	if (!s->firstSkipped)
	{
		// Nothing waits behind a conflict; forget the old candidate unless the
		// scan never reached it (a take earlier in the queue).
		if (!s->took)
			r->ageTask = 0;
		return;
	}
	if (s->firstSkipped != r->ageTask)
	{
		r->ageTask = s->firstSkipped;
		r->ageSince = s->now;
		return;
	}
	if (s->now - r->ageSince < ageAfter || !canReserve)
		return;
	if (NmAdjFindOwned(r, NMADJ_AGE, NMADJ_RESERVED) >= 0)
		return;
	if (NmAdjFindReservationOf(r, s->firstSkipped) >= 0)
		return;   // the bg thread already reserves it
	if (NmAdjAdd(r, s->firstSkippedDesc, s->firstSkipped, NMADJ_RESERVED, NMADJ_AGE, s->now) >= 0)
		out->reservedAge = true;
}

// ---------------------------------------------------------------------------

void NmAdjBgRelease(NmAdjRegistry* r)
{
	int res = NmAdjFindOwned(r, NMADJ_BG, NMADJ_RESERVED);
	if (res >= 0)
		NmAdjFree(r, res);
	r->pin = 0;
}

void NmAdjBgForward(NmAdjRegistry* r, unsigned __int64 head)
{
	if (r->bgoTask == head)
		r->bgoTask = 0;
	int res = NmAdjFindOwned(r, NMADJ_BG, NMADJ_RESERVED);
	if (res >= 0 && r->e[res].task == head)
		NmAdjFree(r, res);
	r->pin = head;
}

static bool BgOnlyType(int type) { return type != 0 && type != 1; }

void NmAdjBgScanBegin(NmAdjBgScan* s, const NmAdjRegistry* r, bool enforce, __int64 now)
{
	// passed[] is filled only up to passedCount; clear the rest by hand.
	memset(s, 0, (size_t)((const char*)s->passed - (const char*)s));
	s->passedCount = 0;
	s->passedFull = false;
	s->runnableBgOnly = 0;
	s->runnableBgOnly2 = 0;
	s->tailSeen = 0;
	s->bgoBlocked = false;
	s->done = false;
	s->enforce = enforce;
	s->now = now;
	s->resIdx = enforce ? NmAdjFindOwned(r, NMADJ_BG, NMADJ_RESERVED) : -1;
	if (s->resIdx >= 0)
		s->resTask = r->e[s->resIdx].task;
}

static bool SameDesc(const NmJobDesc& a, const NmJobDesc& b)
{
	return a.x == b.x && a.y == b.y && a.type == b.type && a.kind == b.kind
	    && a.rx0 == b.rx0 && a.ry0 == b.ry0 && a.rx1 == b.rx1 && a.ry1 == b.ry1;
}

static void NotePassed(NmAdjBgScan* s, const NmJobDesc& d)
{
	for (int i = 0; i < s->passedCount; ++i)
		if (SameDesc(s->passed[i], d))
			return;
	if (s->passedCount == NMADJ_PASSED_CAP)
	{
		s->passedFull = true;
		return;
	}
	s->passed[s->passedCount++] = d;
}

static bool ConflictsPassed(const NmAdjBgScan* s, const NmJobDesc& d)
{
	for (int i = 0; i < s->passedCount; ++i)
		if (NmJobsConflict(d, s->passed[i]))
			return true;
	return false;
}

// The tracked bg-only job's age counts only while it is runnable: seen blocked,
// it restarts.
static void BgOnlyAgeBegin(const NmAdjBgScan* s, NmAdjRegistry* r)
{
	if (s->bgoBlocked)
		r->bgoTask = 0;
}

// A bg-only pick's age: from when it was first seen runnable, or, first seen
// now, from the previous look (it may have been queued any time since).
static void BgOnlyAgeEnd(const NmAdjBgScan* s, NmAdjRegistry* r, unsigned __int64 pick, bool pickBgOnly,
                         NmAdjBgResult* out)
{
	const __int64 prevLook = r->bgLooked ? r->bgLastLook : s->now;
	if (pickBgOnly)
	{
		out->bgOnlyWaited = true;
		out->bgOnlyWait = s->now - (r->bgoTask == pick ? r->bgoSince : prevLook);
	}
	if (r->bgoTask == pick)
		r->bgoTask = 0;
	const unsigned __int64 next = s->runnableBgOnly != pick ? s->runnableBgOnly : s->runnableBgOnly2;
	if (next && next != r->bgoTask)
	{
		r->bgoTask = next;
		r->bgoSince = prevLook;
	}
}

void NmAdjBgLooked(NmAdjRegistry* r, __int64 now)
{
	r->bgLastLook = now;
	r->bgLooked = true;
}

static bool BgScanComplete(const NmAdjBgScan* s)
{
	// Past an overflow no skip can be proven safe; only R is still sought.
	if (s->passedFull && !s->haveAlt)
		return s->resIdx < 0 || s->resSeen;
	if (s->resIdx >= 0)
		return s->resSeen && (s->resRunnable || s->haveAlt);
	return s->haveFirst && (!s->firstBlocked || s->haveAlt);
}

// The first two bg-only jobs seen runnable (one of them may be the pick).
static void NoteBgOnly(NmAdjBgScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                       const NmJobDesc& d, bool blocked)
{
	if (task == r->bgoTask && blocked)
		s->bgoBlocked = true;
	if (blocked || s->runnableBgOnly2 || ConflictsPassed(s, d))
		return;
	if (!s->runnableBgOnly)
		s->runnableBgOnly = task;
	else
		s->runnableBgOnly2 = task;
}

static bool OfferPick(NmAdjBgScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                      const NmJobDesc* d, bool eligible, bool bgOnly)
{
	const bool isRes = s->resIdx >= 0 && task == s->resTask;
	if (isRes)
	{
		s->resSeen = true;
		s->resEligible = eligible;
	}
	if (!eligible)
		return s->done = BgScanComplete(s);

	const bool blocked = NmAdjFindBlocker(r, *d, task, NmAdjOwnResSeq(r, task)) >= 0;
	if (bgOnly)
		NoteBgOnly(s, r, task, *d, blocked);

	if (!s->enforce)
	{
		// Count mode keeps the head-first order: the first eligible job runs.
		s->haveFirst = true;
		s->first = task;
		s->firstDesc = *d;
		s->firstBlocked = blocked;
		return s->done = true;
	}

	if (isRes)
	{
		s->resRunnable = !blocked;
		return s->done = BgScanComplete(s);
	}

	if (s->haveAlt)
		return s->done = BgScanComplete(s);
	if (!s->haveFirst)
	{
		s->haveFirst = true;
		s->first = task;
		s->firstDesc = *d;
		s->firstBlocked = blocked;
		// With a reservation outstanding, F is only a skip candidate.
		if (s->resIdx < 0 || blocked)
		{
			if (blocked)
				NotePassed(s, *d);
			return s->done = BgScanComplete(s);
		}
	}
	else if (blocked)
	{
		NotePassed(s, *d);
		return s->done = BgScanComplete(s);
	}

	// A skip candidate: it may not overtake an eligible job it conflicts with
	// (F among them), and must be bg-only when the reserved job is.
	if (!bgOnly || s->passedFull || ConflictsPassed(s, *d))
	{
		NotePassed(s, *d);
		return s->done = BgScanComplete(s);
	}
	s->haveAlt = true;
	s->alt = task;
	s->altDesc = *d;
	return s->done = BgScanComplete(s);
}

bool NmAdjBgScanWants(const NmAdjBgScan* s, bool bgOnly)
{
	return !s->done || (bgOnly && !s->runnableBgOnly2 && s->tailSeen < NMADJ_TAIL_CAP);
}

bool NmAdjBgScanOffer(NmAdjBgScan* s, const NmAdjRegistry* r, unsigned __int64 task,
                      const NmJobDesc* d, bool eligible, bool bgOnly)
{
	if (!s->done)
		OfferPick(s, r, task, d, eligible, bgOnly);
	else
	{
		// Past the pick, only the bg-only age is still tracked, within the cap.
		++s->tailSeen;
		if (eligible && bgOnly && !s->runnableBgOnly2)
			NoteBgOnly(s, r, task, *d, NmAdjFindBlocker(r, *d, task, NmAdjOwnResSeq(r, task)) >= 0);
	}
	return s->done && (s->runnableBgOnly2 != 0 || s->tailSeen >= NMADJ_TAIL_CAP);
}

static void BgScanEndInner(NmAdjBgScan* s, NmAdjRegistry* r, NmAdjBgResult* out);

void NmAdjBgScanEnd(NmAdjBgScan* s, NmAdjRegistry* r, NmAdjBgResult* out)
{
	BgScanEndInner(s, r, out);
	NmAdjBgLooked(r, s->now);
}

static void BgScanEndInner(NmAdjBgScan* s, NmAdjRegistry* r, NmAdjBgResult* out)
{
	memset(out, 0, sizeof(*out));
	out->claimIdx = -1;
	out->passedFull = s->passedFull;
	r->pin = 0;
	BgOnlyAgeBegin(s, r);

	// R has left the queue or can no longer run here: its priority lapses, and
	// F becomes the job to reserve (the skip was already checked against F).
	if (s->resIdx >= 0 && (!s->resSeen || !s->resEligible))
	{
		NmAdjFree(r, s->resIdx);
		s->resIdx = -1;
		out->droppedRes = true;
	}

	unsigned __int64 pick = 0;
	NmJobDesc pickDesc;
	if (!s->enforce)
	{
		if (!s->haveFirst)
		{
			out->decision = NMADJ_BG_NONE;
			return;
		}
		pick = s->first;
		pickDesc = s->firstDesc;
		out->conflicted = s->firstBlocked;
	}
	else if (s->resIdx >= 0 && s->resRunnable)
	{
		pick = s->resTask;
		pickDesc = r->e[s->resIdx].d;
	}
	else if (s->resIdx < 0 && s->haveFirst && !s->firstBlocked)
	{
		pick = s->first;
		pickDesc = s->firstDesc;
	}
	else
	{
		// Something eligible is blocked. It keeps (R) or gets (F) the
		// reservation; F keeps a workers' age reservation instead if it has one.
		if (s->resIdx < 0 && s->haveFirst && s->firstBlocked && NmAdjOwnResSeq(r, s->first) == 0)
		{
			if (NmAdjAdd(r, s->firstDesc, s->first, NMADJ_RESERVED, NMADJ_BG, s->now) >= 0)
				out->newReservation = true;
		}
		if (!s->haveAlt)
		{
			out->decision = (s->resIdx >= 0 || s->haveFirst) ? NMADJ_BG_WAIT : NMADJ_BG_NONE;
			return;
		}
		pick = s->alt;
		pickDesc = s->altDesc;
		out->skipped = true;
	}

	const bool pickBgOnly = BgOnlyType(pickDesc.type);
	__int64 since = 0;
	if (s->enforce && FreeReservationsOf(r, pick, &since))
	{
		out->deferred = true;
		out->deferredFor = s->now - since;
	}
	if (r->ageTask == pick)
		r->ageTask = 0;
	out->pick = pick;
	out->pickBgOnly = pickBgOnly;
	BgOnlyAgeEnd(s, r, pick, pickBgOnly, out);
	out->claimIdx = NmAdjAdd(r, pickDesc, pick, NMADJ_CLAIMED, NMADJ_BG, s->now);
	if (out->claimIdx < 0)
	{
		out->decision = NMADJ_BG_FULL;
		if (s->enforce)
			return;   // nothing registered and nothing runs: the caller waits
	}
	else
		out->decision = NMADJ_BG_CLAIM;
	r->pin = pickBgOnly ? pick : 0;
}
