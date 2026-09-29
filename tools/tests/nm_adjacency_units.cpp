// Host tests for the navmesh adjacency exclusion: the conflict predicate on
// every job-type pair, the registry, and a simulation of the claim loop
// (workers, the bg thread and the drain) that checks no conflicting pair is
// ever live together, nothing deadlocks and no job starves.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "navmesh/scheduling/nm_adjacency_policy.h"

#include "check.h"

static const float kCell = 4608.0f;

static NmJobDesc E(int x, int y, int type = 0)
{
	NmJobDesc d;
	NmAdjDescribe(x, y, type, type == 4, (unsigned)(x | (y << 8)), NULL, kCell, kCell, &d);
	return d;
}

static NmJobDesc I3(int x, int y)
{
	NmJobDesc d;
	NmAdjDescribe(x, y, 3, false, 0, NULL, kCell, kCell, &d);
	return d;
}

// A type-4 interior with its box: center/half in world units.
static NmJobDesc I4(int x, int y, const float* box)
{
	NmJobDesc d;
	NmAdjDescribe(x, y, 4, true, 0x800b0a19u, box, kCell, kCell, &d);
	return d;
}

static float CellCenter(int c) { return (c - 32) * kCell + kCell * 0.5f; }

static bool C(const NmJobDesc& a, const NmJobDesc& b) { return NmJobsConflict(a, b); }

static void TestCellOf()
{
	Check(NmAdjCellOf(0.0f, kCell) == 32, "world 0 is cell 32");
	Check(NmAdjCellOf(-147456.0f, kCell) == 0, "the grid origin is cell 0");
	Check(NmAdjCellOf(-147456.5f, kCell) == -1, "just west of the origin is -1");
	Check(NmAdjCellOf(kCell - 0.01f, kCell) == 32 && NmAdjCellOf(kCell, kCell) == 33, "cell edge");
}

static void TestPredicate()
{
	// The census pairs.
	Check(C(E(28, 38), E(29, 38)), "28,38 vs 29,38 (the recorded crash pair)");
	Check(C(E(26, 42), E(27, 42)), "26,42 vs 27,42 (reincarnated drop)");
	Check(C(E(24, 40), I3(24, 40)), "24,40 exterior vs the Hideout interior");

	// Exterior pairs.
	Check(C(E(5, 5), E(5, 5)), "same cell (duplicate / late HIT)");
	Check(C(E(5, 5), E(5, 6)) && C(E(5, 5), E(4, 5)), "edge neighbours");
	Check(!C(E(5, 5), E(6, 6)) && !C(E(5, 5), E(4, 4)), "diagonal exteriors never conflict");
	Check(!C(E(5, 5), E(7, 5)), "two cells apart");
	for (int t = 0; t <= 2; ++t)
		Check(C(E(5, 5, t), E(5, 6, 0)), "types 0/1/2 are exteriors");
	Check(C(E(5, 5, 4), E(5, 5, 0)), "type-4 exterior vs a regeneration of its cell");
	Check(C(E(5, 5, 4), E(6, 5, 1)), "type-4 exterior vs an edge neighbour");
	Check(!C(E(5, 5, 4), E(6, 6, 1)), "type-4 exterior vs a diagonal");

	// Exterior / interior: the interior's 8-neighbourhood.
	Check(C(E(5, 5), I3(5, 5)), "interior in the exterior's cell");
	Check(C(E(5, 6), I3(5, 5)) && C(E(4, 5), I3(5, 5)), "interior in an edge neighbour");
	Check(C(E(20, 28), I3(19, 29)), "diagonal: the Weapon Shop 19,29 stitched 20,28");
	Check(C(E(6, 6), I3(5, 5)) && C(E(4, 4), I3(5, 5)), "every diagonal of an interior");
	Check(!C(E(7, 5), I3(5, 5)) && !C(E(5, 3), I3(5, 5)), "two cells from an interior");

	// Interior / interior.
	Check(!C(I3(5, 5), I3(5, 5)), "two type-3 interiors never conflict");
	Check(C(I3(5, 5), I4(5, 5, NULL)), "type-3 vs type-4 in one cell");
	Check(C(I4(5, 5, NULL), I4(5, 5, NULL)), "two type-4 interiors in one cell");
	Check(C(I3(5, 5), I4(6, 5, NULL)), "type-4 zone is the box minimum's cell: one cell off still conflicts");
	Check(!C(I3(5, 5), I4(7, 5, NULL)), "type-4 interior two cells away");

	// Kind of a type-4 task from its output's uid.
	NmJobDesc ext4, int4, unk;
	NmAdjDescribe(29, 38, 4, true, 0x261Du, NULL, kCell, kCell, &ext4);
	NmAdjDescribe(24, 40, 4, true, 0x800b0a19u, NULL, kCell, kCell, &int4);
	NmAdjDescribe(24, 40, 4, false, 0, NULL, kCell, kCell, &unk);
	Check(ext4.kind == NMADJ_KIND_E && int4.kind == NMADJ_KIND_I && unk.kind == NMADJ_KIND_UNKNOWN, "type-4 kind by uid");
	NmJobDesc edge;
	NmAdjDescribe(1, 1, 4, true, 0xFFFFu, NULL, kCell, kCell, &edge);
	Check(edge.kind == NMADJ_KIND_I, "uid 0xFFFF is an interior, as updateBT tests it");
	Check(C(unk, E(25, 41)) && C(unk, I3(24, 40)) && !C(unk, E(26, 40)), "unknown: 8-neighbourhood of both kinds");

	// A type-4 interior's box widens the reach; a type-3 box is never read.
	float box[6] = { CellCenter(10) + 2000.0f, 0.0f, CellCenter(10), 3000.0f, 10.0f, 100.0f };
	NmJobDesc wide = I4(10, 10, box);
	Check(wide.rx0 == 9 && wide.rx1 == 11 && wide.ry0 == 9 && wide.ry1 == 11, "a box inside the neighbourhood");
	box[3] = 9000.0f;   // reaches cell 12
	wide = I4(10, 10, box);
	Check(wide.rx1 == 12 && C(E(12, 10), wide) && !C(E(13, 10), wide), "a box reaching two cells widens the reach");
	box[3] = 1e9f;
	wide = I4(10, 10, box);
	Check(wide.rx0 == 6 && wide.rx1 == 14, "an absurd box is clamped");
	float nanBox[6] = { 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -1.0f };
	wide = I4(10, 10, nanBox);
	Check(wide.rx0 == 9 && wide.rx1 == 11, "an inverted box keeps the neighbourhood");

	// Symmetry over a spread of pairs.
	std::vector<NmJobDesc> all;
	for (int x = 3; x <= 7; ++x)
		for (int y = 3; y <= 7; ++y)
		{
			all.push_back(E(x, y));
			all.push_back(E(x, y, 4));
			all.push_back(I3(x, y));
			all.push_back(I4(x, y, NULL));
		}
	all.push_back(unk);
	bool sym = true;
	for (size_t i = 0; i < all.size(); ++i)
		for (size_t j = 0; j < all.size(); ++j)
			if (C(all[i], all[j]) != C(all[j], all[i]))
				sym = false;
	Check(sym, "the predicate is symmetric");
}

static void TestRegistry()
{
	static NmAdjRegistry r;
	NmAdjInit(&r);
	int a = NmAdjAdd(&r, E(5, 5), 100, NMADJ_CLAIMED, 0, 0);
	Check(a >= 0 && r.live == 1, "add");
	Check(NmAdjFindBlocker(&r, E(5, 6), 101, 0) == a, "claimed neighbour blocks");
	Check(NmAdjFindBlocker(&r, E(6, 6), 101, 0) < 0, "diagonal does not block");
	NmAdjPublish(&r, a);
	Check(r.e[a].state == NMADJ_PUBLISHED && r.published == 1, "publish");
	Check(NmAdjFindBlocker(&r, E(5, 6), 101, 0) == a, "published neighbour still blocks");
	unsigned __int64 done[1] = { 100 };
	Check(NmAdjReleaseDrained(&r, r.pubSeq, done, 1) == 0, "still in done: kept");
	Check(NmAdjReleaseDrained(&r, r.pubSeq - 1, NULL, 0) == 0, "published after the read: kept");
	Check(NmAdjReleaseDrained(&r, r.pubSeq, NULL, 0) == 1 && r.live == 0 && r.high == 0, "drained: freed");

	// Reservations are ordered: the older one never waits for the younger.
	int r1 = NmAdjAdd(&r, E(1, 1), 200, NMADJ_RESERVED, NMADJ_BG, 0);
	int r2 = NmAdjAdd(&r, E(1, 2), 201, NMADJ_RESERVED, NMADJ_AGE, 0);
	Check(r.e[r1].resSeq < r.e[r2].resSeq, "reservation order");
	Check(NmAdjFindBlocker(&r, E(1, 1), 200, r.e[r1].resSeq) < 0, "the older reservation ignores the younger");
	Check(NmAdjFindBlocker(&r, E(1, 2), 201, r.e[r2].resSeq) == r1, "the younger waits for the older");
	Check(NmAdjFindBlocker(&r, E(1, 0), 202, 0) == r1, "an unreserved job respects every reservation");
	NmAdjFree(&r, r1); NmAdjFree(&r, r2);

	// Full.
	for (int i = 0; i < NMADJ_CAP; ++i)
		NmAdjAdd(&r, E(i, 0), 1000 + i, NMADJ_CLAIMED, 0, 0);
	Check(NmAdjAdd(&r, E(0, 60), 5, NMADJ_CLAIMED, 0, 0) < 0, "full refuses");
	NmAdjInit(&r);
}

// ---------------------------------------------------------------------------
// Claim-loop simulation
// ---------------------------------------------------------------------------

struct SimJob
{
	unsigned __int64 id;
	NmJobDesc d;
	bool workerType;      // 0/1: workers or bg; else bg only
	bool content;         // false: the zone was unloaded (bg forwards it, no stitch)
	int  duration;
	int  firstBlocked;    // step it was first seen and not taken, -1 none
	int  claimedAt;
	bool finished;
};

struct Sim
{
	NmAdjRegistry reg;
	std::vector<SimJob> jobs;
	std::vector<int> queue;          // job indices, front first
	std::vector<int> done;           // published, not drained
	std::vector<int> live;           // claimed and running
	int  workers;
	std::vector<int> wJob, wLeft, wIdx;
	int  bgJob, bgLeft, bgIdx;
	bool enforce;
	int  step;
	int  violations;
	int  maxDefer;
	int  pinnedTaken;
	int  would;
	unsigned int rng;
	int  drainEvery;
	int  ageAfter;
	bool bgHeadOnly;          // control: the bg thread offers only the head
	int  bgWaitStart;         // step the current bg wait episode began, -1 none
	int  bgWaitMax;           // longest episode that began before bgWaitCutoff
	int  bgWaitCutoff;
	int  bgWaitedWithWork;    // bg waits while a queued job could have run
	int  bgSkips;
	int  bgPassFull;
	int  bgoMax;              // longest runnable wait of a bg-only job, as the policy reports it
	bool oldSkipRule;         // control: also skip onto a type 0/1 job while a worker is live
};

static unsigned int Rand(Sim* s) { s->rng = s->rng * 1103515245u + 12345u; return (s->rng >> 16) & 0x7FFF; }

static void SimInit(Sim* s, int workers, bool enforce, unsigned int seed)
{
	NmAdjInit(&s->reg);
	s->jobs.clear(); s->queue.clear(); s->done.clear(); s->live.clear();
	s->workers = workers;
	s->wJob.assign(workers, -1); s->wLeft.assign(workers, 0); s->wIdx.assign(workers, -1);
	s->bgJob = -1; s->bgLeft = 0; s->bgIdx = -1;
	s->enforce = enforce;
	s->step = 0; s->violations = 0; s->maxDefer = 0; s->pinnedTaken = 0; s->would = 0;
	s->rng = seed; s->drainEvery = 3; s->ageAfter = 20;
	s->bgHeadOnly = false; s->bgWaitStart = -1; s->bgWaitMax = 0; s->bgWaitCutoff = 0x7FFFFFFF; s->bgWaitedWithWork = 0; s->bgSkips = 0; s->bgPassFull = 0; s->bgoMax = 0; s->oldSkipRule = false;
}

static int AddJob(Sim* s, const NmJobDesc& d, bool workerType, int duration)
{
	SimJob j;
	j.id = 1000 + s->jobs.size();
	j.d = d; j.workerType = workerType; j.content = true;
	j.duration = duration; j.firstBlocked = -1; j.claimedAt = -1; j.finished = false;
	s->jobs.push_back(j);
	s->queue.push_back((int)s->jobs.size() - 1);
	return (int)s->jobs.size() - 1;
}

// Ground truth, independent of the registry: every claimed or undrained job.
static void CheckInvariant(Sim* s)
{
	std::vector<int> act(s->live);
	act.insert(act.end(), s->done.begin(), s->done.end());
	for (size_t i = 0; i < act.size(); ++i)
		for (size_t k = i + 1; k < act.size(); ++k)
			if (NmJobsConflict(s->jobs[act[i]].d, s->jobs[act[k]].d))
				++s->violations;
}

static void RemoveQueued(Sim* s, int j)
{
	for (size_t i = 0; i < s->queue.size(); ++i)
		if (s->queue[i] == j) { s->queue.erase(s->queue.begin() + i); return; }
}

static void Claimed(Sim* s, int j)
{
	SimJob& job = s->jobs[j];
	job.claimedAt = s->step;
	if (job.firstBlocked >= 0 && s->step - job.firstBlocked > s->maxDefer)
		s->maxDefer = s->step - job.firstBlocked;
	RemoveQueued(s, j);
	s->live.push_back(j);
}

static void Finish(Sim* s, int j, int idx)
{
	for (size_t i = 0; i < s->live.size(); ++i)
		if (s->live[i] == j) { s->live.erase(s->live.begin() + i); break; }
	if (idx >= 0)
		NmAdjPublish(&s->reg, idx);
	s->done.push_back(j);
}

static void WorkerStep(Sim* s, int w)
{
	if (s->wJob[w] >= 0)
	{
		if (--s->wLeft[w] > 0) return;
		Finish(s, s->wJob[w], s->wIdx[w]);
		s->wJob[w] = -1;
		return;
	}
	NmAdjScan scan;
	NmAdjScanBegin(&scan, &s->reg, s->enforce, s->step);
	int taken = -1;
	for (size_t i = 0; i < s->queue.size(); ++i)
	{
		int j = s->queue[i];
		SimJob& job = s->jobs[j];
		bool eligible = job.workerType && job.content && job.id != s->reg.pin;
		if (taken >= 0) { NmAdjScanObserve(&scan, job.id, eligible); if (!NmAdjScanWantsRest(&scan)) break; continue; }
		if (!eligible) { NmAdjScanObserve(&scan, job.id, false); continue; }
		NmAdjOffer o = NmAdjScanOffer(&scan, &s->reg, job.id, &job.d, true);
		if (o == NMADJ_OFFER_SKIP)
		{
			if (job.firstBlocked < 0) job.firstBlocked = s->step;
			continue;
		}
		taken = j;
		if (!NmAdjScanWantsRest(&scan)) break;
	}
	NmAdjScanResult r;
	NmAdjScanEnd(&scan, &s->reg, w, taken >= 0 ? &s->jobs[taken].d : NULL, s->ageAfter, s->workers > 0, &r);
	if (taken < 0 || r.refused) return;
	if (scan.tookConflicting) ++s->would;
	if (s->jobs[taken].id == s->reg.pin) ++s->pinnedTaken;
	Claimed(s, taken);
	s->wJob[w] = taken; s->wLeft[w] = s->jobs[taken].duration; s->wIdx[w] = r.claimIdx;
}

// Ground truth for a bg wait: no queued job could have run. A job can run when
// it conflicts with nothing live or undrained, with no reservation it must
// respect and with no eligible job queued ahead of it (the reserved job
// excepted), and passes the bg-only rule while the reserved job is bg-only.
static bool BgHadRunnableWork(const Sim* s)
{
	int res = NmAdjFindOwned(&s->reg, NMADJ_BG, NMADJ_RESERVED);
	// Past a blocked job only a bg-only job may run (a skip); the first
	// eligible job and the reserved one may be of any type.
	bool firstSeen = false;
	for (size_t q = 0; q < s->queue.size(); ++q)
	{
		const SimJob& j = s->jobs[s->queue[q]];
		if (!j.content)
			continue;
		const bool isRes = res >= 0 && s->reg.e[res].task == j.id;
		const bool isFirst = !firstSeen;
		firstSeen = true;
		bool blocked = false;
		for (size_t p = 0; p < q && !isRes && !blocked; ++p)
		{
			const SimJob& ahead = s->jobs[s->queue[p]];
			blocked = ahead.content && NmJobsConflict(j.d, ahead.d);
		}
		for (size_t k = 0; k < s->live.size() && !blocked; ++k)
			blocked = NmJobsConflict(j.d, s->jobs[s->live[k]].d);
		for (size_t k = 0; k < s->done.size() && !blocked; ++k)
			blocked = NmJobsConflict(j.d, s->jobs[s->done[k]].d);
		long own = NmAdjOwnResSeq(&s->reg, j.id);
		for (int i = 0; i < s->reg.high && !blocked; ++i)
		{
			const NmAdjEntry& e = s->reg.e[i];
			if (e.state != NMADJ_RESERVED || e.task == j.id || (own && e.resSeq > own))
				continue;
			blocked = NmJobsConflict(j.d, e.d);
		}
		if (blocked)
			continue;
		if (!isRes && !isFirst && j.workerType)
			continue;
		return true;
	}
	return false;
}

static void EndBgEpisode(Sim* s)
{
	if (s->bgWaitStart < 0)
		return;
	int len = s->step - s->bgWaitStart;
	if (s->bgWaitStart < s->bgWaitCutoff && len > s->bgWaitMax)
		s->bgWaitMax = len;
	s->bgWaitStart = -1;
}

// Control for the skip rule: the earlier policy also skipped onto a type 0/1
// job while any worker was live. Emulated after a WAIT: the first unblocked
// type 0/1 job that overtakes no conflicting eligible job.
static void OldRuleTypeSkip(Sim* s)
{
	for (size_t q = 0; q < s->queue.size(); ++q)
	{
		const SimJob& j = s->jobs[s->queue[q]];
		if (!j.content || !j.workerType)
			continue;
		if (NmAdjFindBlocker(&s->reg, j.d, j.id, NmAdjOwnResSeq(&s->reg, j.id)) >= 0)
			continue;
		bool overtakes = false;
		for (size_t p = 0; p < q && !overtakes; ++p)
			overtakes = s->jobs[s->queue[p]].content && NmJobsConflict(j.d, s->jobs[s->queue[p]].d);
		if (overtakes)
			continue;
		int idx = NmAdjAdd(&s->reg, j.d, j.id, NMADJ_CLAIMED, NMADJ_BG, s->step);
		int ji = s->queue[q];
		Claimed(s, ji);
		s->bgJob = ji; s->bgLeft = s->jobs[ji].duration; s->bgIdx = idx;
		return;
	}
}

static void NoteFirstBlocked(Sim* s, const NmAdjBgScan* scan)
{
	if (!scan->haveFirst || !scan->firstBlocked)
		return;
	SimJob& f = s->jobs[(size_t)(scan->first - 1000)];
	if (f.firstBlocked < 0) f.firstBlocked = s->step;
}

static void BgStep(Sim* s)
{
	if (s->bgJob >= 0)
	{
		if (--s->bgLeft > 0) return;
		Finish(s, s->bgJob, s->bgIdx);
		s->reg.pin = 0;
		s->bgJob = -1;
		return;
	}
	if (s->queue.empty())
	{
		// hook_dispatchJob's unlocked empty-queue return: it records a look and
		// nothing else (no lock, the reservation stays).
		NmAdjBgLooked(&s->reg, s->step);
		EndBgEpisode(s);
		return;
	}
	int head = s->queue.front();
	if (!s->jobs[head].content)
	{
		// Forwarded to the original, which drops it: nothing registered.
		NmAdjBgForward(&s->reg, s->jobs[head].id);
		NmAdjBgLooked(&s->reg, s->step);
		RemoveQueued(s, head);
		s->jobs[head].finished = true;
		s->reg.pin = 0;
		EndBgEpisode(s);
		return;
	}
	NmAdjBgScan scan;
	NmAdjBgScanBegin(&scan, &s->reg, s->enforce, s->step);
	for (size_t i = 0; i < s->queue.size(); ++i)
	{
		const SimJob& job = s->jobs[s->queue[i]];
		if (NmAdjBgScanOffer(&scan, &s->reg, job.id, &job.d, job.content, !job.workerType))
			break;
		if (s->bgHeadOnly)
			break;   // control: the old head-only bg thread
	}
	NmAdjBgResult r;
	NmAdjBgScanEnd(&scan, &s->reg, &r);
	if (r.decision != NMADJ_BG_CLAIM && !(r.decision == NMADJ_BG_FULL && !s->enforce))
	{
		if (r.decision == NMADJ_BG_NONE) { EndBgEpisode(s); return; }
		if (s->bgWaitStart < 0) s->bgWaitStart = s->step;
		// Past the passed-job cap the scan stops looking for a skip: a
		// deliberate wait, counted apart.
		if (r.passedFull) ++s->bgPassFull;
		else if (BgHadRunnableWork(s)) ++s->bgWaitedWithWork;
		NoteFirstBlocked(s, &scan);
		if (s->oldSkipRule && s->workers > 0)
			OldRuleTypeSkip(s);
		return;
	}
	NoteFirstBlocked(s, &scan);
	EndBgEpisode(s);
	if (r.bgOnlyWaited && r.bgOnlyWait > s->bgoMax) s->bgoMax = (int)r.bgOnlyWait;
	if (r.skipped) ++s->bgSkips;
	if (r.conflicted) ++s->would;
	int j = (int)(r.pick - 1000);
	Claimed(s, j);
	s->bgJob = j; s->bgLeft = s->jobs[j].duration; s->bgIdx = r.claimIdx;
}

static void DrainStep(Sim* s)
{
	// The path thread pops everything in done, then the observer runs.
	for (size_t i = 0; i < s->done.size(); ++i)
		s->jobs[s->done[i]].finished = true;
	s->done.clear();
	NmAdjReleaseDrained(&s->reg, s->reg.pubSeq, NULL, 0);
}

static bool AllFinished(const Sim* s)
{
	for (size_t i = 0; i < s->jobs.size(); ++i)
		if (!s->jobs[i].finished) return false;
	return true;
}

// Runs actors in a random order each step. Returns false on no progress.
static bool SimRun(Sim* s, int maxSteps, void (*arrivals)(Sim*) = NULL)
{
	for (s->step = 0; s->step < maxSteps; ++s->step)
	{
		if (arrivals) arrivals(s);
		int order[16], n = 0;
		for (int w = 0; w < s->workers; ++w) order[n++] = w;
		order[n++] = -1;   // bg
		order[n++] = -2;   // drain
		for (int i = n - 1; i > 0; --i) { int k = Rand(s) % (i + 1); int t = order[i]; order[i] = order[k]; order[k] = t; }
		for (int i = 0; i < n; ++i)
		{
			if (order[i] >= 0) WorkerStep(s, order[i]);
			else if (order[i] == -1) BgStep(s);
			else if (s->step % s->drainEvery == 0) DrainStep(s);
			CheckInvariant(s);
		}
		if (AllFinished(s) && s->reg.live == 0 && !arrivals) return true;
	}
	return AllFinished(s);
}

// A dense block of mixed jobs, the census's hash-mismatch rows included.
static void FillMixed(Sim* s, unsigned int seed)
{
	s->rng = seed;
	for (int k = 0; k < 160; ++k)
	{
		int x = 28 + (int)(Rand(s) % 5), y = 37 + (int)(Rand(s) % 5);
		int kind = Rand(s) % 10;
		int dur = 1 + (int)(Rand(s) % 12);
		if (kind < 6)       AddJob(s, E(x, y, Rand(s) % 2), true, dur);
		else if (kind < 8)  AddJob(s, I3(x, y), false, dur);
		else if (kind < 9)  AddJob(s, I4(x, y, NULL), false, dur);
		else                AddJob(s, E(x, y, 2 + 2 * (Rand(s) % 2)), false, dur);
	}
}

static void TestSimMixed()
{
	bool deferredSomewhere = false;
	for (unsigned int seed = 1; seed <= 40; ++seed)
	{
		for (int workers = 0; workers <= 6; workers += 3)
		{
			static Sim s;
			SimInit(&s, workers, true, seed);
			FillMixed(&s, seed * 7919u);
			s.rng = seed;
			bool ok = SimRun(&s, 20000);
			char msg[128];
			sprintf_s(msg, sizeof(msg), "mixed seed %u workers %d: completes", seed, workers);
			Check(ok, msg);
			sprintf_s(msg, sizeof(msg), "mixed seed %u workers %d: no conflicting pair live (%d)", seed, workers, s.violations);
			Check(s.violations == 0, msg);
			Check(s.pinnedTaken == 0, "a worker never takes the pinned head");
			sprintf_s(msg, sizeof(msg), "mixed seed %u workers %d: the bg thread never waits while a job could run (%d)", seed, workers, s.bgWaitedWithWork);
			Check(s.bgWaitedWithWork == 0, msg);
			sprintf_s(msg, sizeof(msg), "mixed seed %u workers %d: no scan reaches the passed-job cap (%d)", seed, workers, s.bgPassFull);
			Check(s.bgPassFull == 0, msg);
			Check(s.reg.live == 0 && s.reg.pin == 0, "registry empty at the end");
			if (workers == 6)
				deferredSomewhere = deferredSomewhere || s.maxDefer > 0;
		}
	}
	Check(deferredSomewhere, "the mixed runs actually deferred conflicting jobs");
}

static void TestSimCountMode()
{
	static Sim s;
	SimInit(&s, 6, false, 3);
	FillMixed(&s, 12345u);
	s.rng = 3;
	Check(SimRun(&s, 20000), "count mode completes");
	Check(s.violations > 0 && s.would > 0, "count mode lets conflicts through and counts them");
}

// A job whose neighbours keep arriving and keep being claimed ahead of it.
static int s_starveTarget = -1;
static void StarveArrivals(Sim* s)
{
	if (s->step < 400 && s->step % 2 == 0)
	{
		static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
		int k = (s->step / 2) % 4;
		AddJob(s, E(10 + dx[k], 10 + dy[k]), true, 6);
	}
}

static void RunStarvation(Sim* sp, int ageAfter)
{
	Sim& s = *sp;
	SimInit(&s, 6, true, 11);
	s.ageAfter = ageAfter;
	s.drainEvery = 2;
	AddJob(&s, I3(40, 40), false, 400);
	AddJob(&s, E(11, 10), true, 30);
	s_starveTarget = AddJob(&s, E(10, 10), true, 3);
	SimRun(&s, 3000, StarveArrivals);
}

static void TestSimStarvation()
{
	// Control: without ageing the stream of neighbours holds the target off
	// for as long as it lasts, so the bound below is a real test.
	static Sim c;
	RunStarvation(&c, 1000000);
	Check(c.jobs[s_starveTarget].claimedAt >= 300, "control: without ageing the target starves while arrivals last");

	static Sim s;
	SimInit(&s, 6, true, 11);
	s.drainEvery = 2;
	// The bg thread is busy with a long interior elsewhere, so only the
	// workers' ageing can protect the target; something long runs beside it.
	AddJob(&s, I3(40, 40), false, 400);
	AddJob(&s, E(11, 10), true, 30);
	s_starveTarget = AddJob(&s, E(10, 10), true, 3);
	bool ok = SimRun(&s, 3000, StarveArrivals);
	Check(ok, "starvation: everything completes");
	Check(s.violations == 0, "starvation: no conflicting pair live");
	const SimJob& t = s.jobs[s_starveTarget];
	char msg[128];
	sprintf_s(msg, sizeof(msg), "starvation: the target ran at step %d (bounded by age + one window)", t.claimedAt);
	Check(t.claimedAt >= 0 && t.claimedAt < 30 + s.ageAfter + 40, msg);
	Check(s.maxDefer < 200, "starvation: longest deferral is bounded");
}

// The bg head (an interior) and a worker's aged job conflict with each other;
// both reservations exist at once and neither may wait for the other.
static void TestSimTwoReservations()
{
	static Sim s;
	SimInit(&s, 2, true, 5);
	s.drainEvery = 1;
	AddJob(&s, E(21, 21), true, 40);         // blocks both below
	AddJob(&s, I3(20, 20), false, 5);        // bg head once the first is claimed
	AddJob(&s, E(21, 20), true, 5);          // aged by the workers
	bool ok = SimRun(&s, 2000);
	Check(ok && s.violations == 0, "two mutually conflicting reservations resolve");
}

// The workers' aged job loses its zone content; its reservation must go.
static void TestSimStaleReservation()
{
	static Sim s;
	SimInit(&s, 3, true, 9);
	s.drainEvery = 1;
	AddJob(&s, E(5, 6), true, 60);
	int victim = AddJob(&s, E(5, 5), true, 3);
	int behind = AddJob(&s, E(4, 5), true, 3);
	(void)behind;
	for (s.step = 0; s.step < 40; ++s.step)
	{
		for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
		DrainStep(&s);
	}
	Check(NmAdjFindOwned(&s.reg, NMADJ_AGE, NMADJ_RESERVED) >= 0, "the aged job is reserved");
	s.jobs[victim].content = false;
	for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
	Check(NmAdjFindOwned(&s.reg, NMADJ_AGE, NMADJ_RESERVED) < 0, "an unclaimable reservation is dropped");
	bool ok = SimRun(&s, 2000);
	Check(ok && s.violations == 0, "and the rest completes");
}

// No worker alive: nothing is age-reserved, and the bg thread alone finishes.
static void TestSimNoWorkers()
{
	static Sim s;
	SimInit(&s, 0, true, 2);
	FillMixed(&s, 777u);
	s.rng = 2;
	Check(SimRun(&s, 20000), "zero workers: the bg thread finishes the queue");
	Check(s.violations == 0, "zero workers: no conflicting pair live");
	Check(NmAdjFindOwned(&s.reg, NMADJ_AGE, NMADJ_RESERVED) < 0, "zero workers: no age reservation");
}

// The drain stalls (escape menu): conflicting claims wait, others proceed,
// and everything resumes when it runs again.
static void TestSimDrainStall()
{
	static Sim s;
	SimInit(&s, 4, true, 4);
	AddJob(&s, E(1, 1), true, 2);
	AddJob(&s, E(1, 2), true, 2);
	AddJob(&s, E(9, 9), true, 2);
	s.drainEvery = 1000000;   // no drain
	for (s.step = 1; s.step < 50; ++s.step)
	{
		for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
		BgStep(&s);
		CheckInvariant(&s);
	}
	Check(s.jobs[1].claimedAt < 0 && s.jobs[2].claimedAt >= 0, "stalled drain: the neighbour waits, the far job runs");
	s.drainEvery = 1;
	Check(SimRun(&s, 500) && s.violations == 0, "the drain resumes and everything completes");
}

// The slow cell: a worker holds (22,40) for 550 steps (the route's ~11 s tile)
// while its four edge neighbours keep arriving, workers' and bg-only alike,
// and bg-only work elsewhere arrives throughout.
static int s_slowCell = -1;
static int s_busyR = -1;
static std::vector<int> s_farBgJobs;
static void SlowCellArrivals(Sim* s)
{
	if (s->step >= 450)
		return;
	static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
	if (s->step % 3 == 0)
	{
		int k = (s->step / 3) % 4;
		if ((s->step / 3) % 2 == 0)
			AddJob(s, E(22 + dx[k], 40 + dy[k], (s->step / 6) % 2), true, 3);
		else
			AddJob(s, E(22 + dx[k], 40 + dy[k], 2), false, 2);
	}
	if (s->step % 5 == 0)
		s_farBgJobs.push_back(AddJob(s, I3(5 + (s->step / 5) % 3, 5), false, 2));
	if (s->step % 7 == 0)
		AddJob(s, E(10 + (s->step / 7) % 4 * 2, 30), true, 4);
}

struct SlowCellOutcome
{
	bool completed;
	int  slowEnd;          // step the slow cell was drained
	int  farWorst;         // longest wait of a far bg-only job, in steps
	int  reservedAtEnd;    // the bg reservation's job when the slow cell ended
	int  reservedRanAt;
};

static void RunSlowCell(Sim* sp, bool headOnly, SlowCellOutcome* out)
{
	Sim& s = *sp;
	SimInit(&s, 6, true, 21);
	s.drainEvery = 2;
	s.bgHeadOnly = headOnly;
	// After the arrivals stop only the slow cell's neighbours are left, and the
	// bg thread rightly idles until it drains.
	s.bgWaitCutoff = 445;   // the last far arrival
	s_farBgJobs.clear();
	s_slowCell = AddJob(&s, E(22, 40), true, 550);
	WorkerStep(&s, 0);   // a worker has the slow cell before anything else runs

	out->slowEnd = -1;
	out->reservedAtEnd = -1;
	out->reservedRanAt = -1;
	bool done = false;
	for (s.step = 1; s.step < 6000 && !done; ++s.step)
	{
		SlowCellArrivals(&s);
		int order[16], n = 0;
		for (int w = 0; w < s.workers; ++w) order[n++] = w;
		order[n++] = -1;
		order[n++] = -2;
		for (int i = n - 1; i > 0; --i) { int k = Rand(&s) % (i + 1); int t = order[i]; order[i] = order[k]; order[k] = t; }
		for (int i = 0; i < n; ++i)
		{
			if (order[i] >= 0) WorkerStep(&s, order[i]);
			else if (order[i] == -1) BgStep(&s);
			else if (s.step % s.drainEvery == 0) DrainStep(&s);
			CheckInvariant(&s);
		}
		if (out->slowEnd < 0 && s.jobs[s_slowCell].finished)
		{
			out->slowEnd = s.step;
			int res = NmAdjFindOwned(&s.reg, NMADJ_BG, NMADJ_RESERVED);
			if (res >= 0)
				out->reservedAtEnd = (int)(s.reg.e[res].task - 1000);
		}
		if (out->reservedAtEnd >= 0 && out->reservedRanAt < 0 && s.jobs[out->reservedAtEnd].claimedAt >= 0)
			out->reservedRanAt = s.jobs[out->reservedAtEnd].claimedAt;
		done = s.step > 450 && AllFinished(&s);
	}
	out->completed = done;
	out->farWorst = 0;
	for (size_t i = 0; i < s_farBgJobs.size(); ++i)
	{
		const SimJob& j = s.jobs[s_farBgJobs[i]];
		int arrived = -1;
		// Far jobs arrive at step 5k; their index order matches arrival order.
		arrived = (int)i * 5;
		int waited = (j.claimedAt >= 0 ? j.claimedAt : s.step) - arrived;
		if (waited > out->farWorst) out->farWorst = waited;
	}
}

static void TestSimSlowCell()
{
	// Control: the old head-only bg thread waits behind a blocked neighbour
	// while bg-only work elsewhere sits queued.
	static Sim c;
	SlowCellOutcome co;
	RunSlowCell(&c, true, &co);
	char msg[160];
	sprintf_s(msg, sizeof(msg), "slow cell control: head-only bg waits with work queued (%d polls, episode %d steps, far job %d steps)",
	          c.bgWaitedWithWork, c.bgWaitMax, co.farWorst);
	Check(c.bgWaitedWithWork > 0 && c.bgWaitMax > 100 && co.farWorst > 100, msg);

	static Sim s;
	SlowCellOutcome o;
	RunSlowCell(&s, false, &o);
	Check(o.completed, "slow cell: everything completes");
	sprintf_s(msg, sizeof(msg), "slow cell: no conflicting pair live (%d)", s.violations);
	Check(s.violations == 0, msg);
	sprintf_s(msg, sizeof(msg), "slow cell: the bg thread never waits while a job could run (%d)", s.bgWaitedWithWork);
	Check(s.bgWaitedWithWork == 0, msg);
	Check(s.bgSkips > 0, "slow cell: the bg thread took later jobs past the blocked one");
	Check(s.bgPassFull == 0, "slow cell: no scan reaches the passed-job cap");
	sprintf_s(msg, sizeof(msg), "slow cell: far bg-only work runs during the slow cell (worst wait %d steps)", o.farWorst);
	Check(o.farWorst <= 10, msg);
	sprintf_s(msg, sizeof(msg), "slow cell: the bg-only runnable age stays short (%d steps)", s.bgoMax);
	Check(s.bgoMax <= 10, msg);
	sprintf_s(msg, sizeof(msg), "slow cell: bg wait episodes stay short while other work arrives (%d steps)", s.bgWaitMax);
	Check(s.bgWaitMax <= 10, msg);
	Check(o.slowEnd > 550, "slow cell: the slow cell ran its full length");
	sprintf_s(msg, sizeof(msg), "slow cell: the reserved neighbour (job %d) runs within a drain and one short job of the end (%d -> %d)",
	          o.reservedAtEnd, o.slowEnd, o.reservedRanAt);
	Check(o.reservedAtEnd >= 0 && o.reservedRanAt >= 0 && o.reservedRanAt <= o.slowEnd + s.drainEvery + 3 + 2, msg);
	Check(s.pinnedTaken == 0, "slow cell: a worker never takes the pinned node");
	Check(s.reg.live == 0 && s.reg.pin == 0, "slow cell: registry empty at the end");
}

// R is type 0/1 and every worker is busy: the bg thread must not skip onto a
// type 0/1 job (it could be a long MISS), or R waits it out once it clears.
static void RunBusyWorkers(Sim* sp, bool oldRule)
{
	Sim& s = *sp;
	SimInit(&s, 3, true, 8);
	// The drain runs after the workers on every third step, so the worker that
	// finishes B scans once while B is still undrained.
	s.drainEvery = 3;
	s.oldSkipRule = oldRule;
	AddJob(&s, E(5, 30), true, 600);           // L0
	AddJob(&s, E(9, 30), true, 600);           // L1
	AddJob(&s, E(21, 20), true, 20);           // B: blocks R
	s.step = 0;
	for (int w = 0; w < 3; ++w) WorkerStep(&s, w);
	s_busyR = AddJob(&s, E(20, 20), true, 3);  // R
	AddJob(&s, E(13, 30), true, 600);          // L2: w2's next job
	AddJob(&s, E(40, 10), true, 400);          // M: a long MISS
	AddJob(&s, I3(50, 50), false, 3);          // bg-only work elsewhere
	for (s.step = 1; s.step < 2000 && !AllFinished(&s); ++s.step)
	{
		BgStep(&s);
		for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
		if (s.step % s.drainEvery == 0) DrainStep(&s);
		CheckInvariant(&s);
	}
}

static void TestBusyWorkersBgOnlySkip()
{
	static Sim c;
	RunBusyWorkers(&c, true);
	char msg[160];
	sprintf_s(msg, sizeof(msg), "control: with a type 0/1 skip while every worker is busy, R waits out a long job (step %d)",
	          c.jobs[s_busyR].claimedAt);
	Check(c.jobs[s_busyR].claimedAt > 300, msg);

	static Sim s;
	RunBusyWorkers(&s, false);
	sprintf_s(msg, sizeof(msg), "every worker busy: R runs one drain after its blocker (step %d)", s.jobs[s_busyR].claimedAt);
	Check(s.jobs[s_busyR].claimedAt > 0 && s.jobs[s_busyR].claimedAt <= 20 + 3 + 2, msg);
	sprintf_s(msg, sizeof(msg), "every worker busy: the bg-only age stays short (%d steps)", s.bgoMax);
	Check(s.bgoMax <= 5, msg);
	Check(s.violations == 0 && s.bgWaitedWithWork == 0, "every worker busy: no conflict, no wait with runnable work");
}

// The bg-only age can fail: the bg thread takes a long type 0/1 job at the head
// (vanilla order, nothing blocked) while bg-only work waits behind it.
static void TestBgOnlyAgeCanFail()
{
	static Sim s;
	SimInit(&s, 2, true, 3);
	s.drainEvery = 1;
	AddJob(&s, E(5, 30), true, 300);   // two long jobs occupy the workers
	AddJob(&s, E(9, 30), true, 300);
	s.step = 0;
	for (int w = 0; w < 2; ++w) WorkerStep(&s, w);
	AddJob(&s, E(40, 10), true, 400);  // the head: a long MISS the bg thread takes
	AddJob(&s, I3(50, 50), false, 3);  // bg-only work behind it
	for (s.step = 1; s.step < 2000 && !AllFinished(&s); ++s.step)
	{
		BgStep(&s);
		for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
		DrainStep(&s);
		CheckInvariant(&s);
	}
	char msg[128];
	sprintf_s(msg, sizeof(msg), "the bg-only age reads the stall behind a long bg job (%d steps)", s.bgoMax);
	Check(s.bgoMax >= 390, msg);

	// The same stall with the interior queued while the bg thread is inside the
	// long job: never seen before the pick, it still reads from the look before.
	static Sim m;
	SimInit(&m, 2, true, 3);
	m.drainEvery = 1;
	AddJob(&m, E(5, 30), true, 300);
	AddJob(&m, E(9, 30), true, 300);
	m.step = 0;
	for (int w = 0; w < 2; ++w) WorkerStep(&m, w);
	AddJob(&m, E(40, 10), true, 400);
	int late = -1;
	for (m.step = 1; m.step < 2000 && (late < 0 || !AllFinished(&m)); ++m.step)
	{
		if (m.step == 50)
			late = AddJob(&m, I3(50, 50), false, 3);
		BgStep(&m);
		for (int w = 0; w < m.workers; ++w) WorkerStep(&m, w);
		DrainStep(&m);
		CheckInvariant(&m);
	}
	sprintf_s(msg, sizeof(msg), "a bg-only job queued mid-job reads the stall (%d steps, claimed at %d)",
	          m.bgoMax, late >= 0 ? m.jobs[late].claimedAt : -1);
	Check(m.bgoMax >= 390 && late >= 0 && m.jobs[late].claimedAt >= 400, msg);
}

// A long idle stretch of empty-queue returns, then a bg-only job that is
// picked at once: its age is one look, not the idle stretch.
static void TestBgOnlyAgeAfterIdle()
{
	static Sim s;
	SimInit(&s, 2, true, 4);
	s.drainEvery = 1;
	AddJob(&s, I3(20, 20), false, 2);
	for (s.step = 0; s.step < 5; ++s.step) { BgStep(&s); DrainStep(&s); }   // a scan, then idle
	for (; s.step < 6000; ++s.step) BgStep(&s);                              // 6,000 idle returns
	int j = AddJob(&s, I3(30, 30), false, 2);
	BgStep(&s);
	char msg[128];
	sprintf_s(msg, sizeof(msg), "after a long idle stretch a bg-only job picked at once reads one look (%d steps)", s.bgoMax);
	Check(s.jobs[j].claimedAt == 6000 && s.bgoMax <= 1, msg);
}

// A worker that claims the bg thread's reserved job ends that reservation and
// reports the deferral.
static void TestWorkerTakesBgReservation()
{
	static NmAdjRegistry r;
	NmAdjInit(&r);
	int blocker = NmAdjAdd(&r, E(5, 6), 900, NMADJ_CLAIMED, 0, 0);
	NmJobDesc h = E(5, 5);
	NmAdjBgScan bs;
	NmAdjBgScanBegin(&bs, &r, true, 10);
	NmAdjBgScanOffer(&bs, &r, 100, &h, true, false);
	NmAdjBgResult br;
	NmAdjBgScanEnd(&bs, &r, &br);
	Check(br.decision == NMADJ_BG_WAIT && br.newReservation && r.pin == 0, "bg reserves a blocked head and pins nothing");
	NmAdjFree(&r, blocker);

	NmAdjScan ws;
	NmAdjScanBegin(&ws, &r, true, 50);
	Check(NmAdjScanOffer(&ws, &r, 100, &h, true) == NMADJ_OFFER_TAKE, "a worker may take the unpinned reserved job");
	NmAdjScanResult wr;
	NmAdjScanEnd(&ws, &r, 1, &h, 1000, true, &wr);
	Check(wr.claimIdx >= 0 && NmAdjFindOwned(&r, NMADJ_BG, NMADJ_RESERVED) < 0, "the claim frees the bg reservation");
	Check(wr.deferredTaken && wr.deferredFor == 40, "and reports the deferral");
}

// The bg thread's reserved job H (type 0/1) and a younger age reservation that
// conflicts with it. With the bg thread busy on a long job, a worker must still
// run H once its blocker drains: H's reservation is the older.
static void TestBgReservationVsYoungerAge()
{
	static Sim s;
	SimInit(&s, 2, true, 6);
	s.drainEvery = 1;
	s.ageAfter = 5;
	int x = AddJob(&s, E(21, 21), true, 40);
	int h = AddJob(&s, E(21, 20), true, 3);
	int l = AddJob(&s, I3(40, 40), false, 300);
	s.step = 0;
	WorkerStep(&s, 0);                 // X
	BgStep(&s);                        // reserves H, runs L
	// A arrives and the prioritizer puts it first, so the workers skip it
	// before H and age it: a reservation younger than H's.
	int a = AddJob(&s, E(20, 20), true, 3);
	RemoveQueued(&s, a);
	s.queue.insert(s.queue.begin(), a);
	Check(s.jobs[x].claimedAt == 0 && s.jobs[l].claimedAt == 0, "setup: X on a worker, L on the bg thread");
	int res = NmAdjFindOwned(&s.reg, NMADJ_BG, NMADJ_RESERVED);
	Check(res >= 0 && s.reg.e[res].task == s.jobs[h].id, "setup: H is the bg reservation");
	bool aged = false;
	for (s.step = 1; s.step < 400 && !s.jobs[h].finished; ++s.step)
	{
		for (int w = 0; w < s.workers; ++w) WorkerStep(&s, w);
		BgStep(&s);
		DrainStep(&s);
		CheckInvariant(&s);
		int age = NmAdjFindOwned(&s.reg, NMADJ_AGE, NMADJ_RESERVED);
		if (age >= 0 && s.reg.e[age].task == s.jobs[a].id)
			aged = true;
	}
	Check(aged, "the conflicting job behind H was age-reserved");
	char msg[128];
	sprintf_s(msg, sizeof(msg), "a worker ran H while the bg thread was busy (step %d)", s.jobs[h].claimedAt);
	Check(s.jobs[h].claimedAt > 0 && s.jobs[h].claimedAt < 300 && s.bgJob == l, msg);
	Check(s.violations == 0, "no conflicting pair live");
}

// A skip never overtakes an eligible job it conflicts with: an interior G that
// is blocked, then a carve H in the same cell that is not.
static void TestBgSkipKeepsConflictOrder()
{
	static NmAdjRegistry r;
	NmAdjInit(&r);
	NmAdjAdd(&r, E(20, 21), 900, NMADJ_CLAIMED, 0, 0);   // blocks F
	NmAdjAdd(&r, E(6, 6), 901, NMADJ_CLAIMED, 1, 0);     // blocks G (its 3x3), not H (diagonal)
	NmJobDesc f = E(20, 20), g = I3(5, 5), h = E(5, 5, 2), far = I3(30, 30);
	NmAdjBgScan bs;
	NmAdjBgResult br;
	NmAdjBgScanBegin(&bs, &r, true, 0);
	bool stop = NmAdjBgScanOffer(&bs, &r, 100, &f, true, false)
	         || NmAdjBgScanOffer(&bs, &r, 101, &g, true, true)
	         || NmAdjBgScanOffer(&bs, &r, 102, &h, true, true);
	NmAdjBgScanEnd(&bs, &r, &br);
	Check(!stop && br.decision == NMADJ_BG_WAIT && br.pick == 0, "a carve does not overtake the blocked interior of its cell");
	// A job that conflicts with nothing passed is still a skip.
	NmAdjBgScanBegin(&bs, &r, true, 1);
	NmAdjBgScanOffer(&bs, &r, 100, &f, true, false);
	NmAdjBgScanOffer(&bs, &r, 101, &g, true, true);
	NmAdjBgScanOffer(&bs, &r, 102, &h, true, true);
	NmAdjBgScanOffer(&bs, &r, 103, &far, true, true);
	NmAdjBgScanEnd(&bs, &r, &br);
	Check(br.decision == NMADJ_BG_CLAIM && br.pick == 103 && br.skipped, "an unrelated later job is still taken");
}

// A bg reservation whose job has left the queue lapses; the next blocked job
// takes it over.
static void TestBgStaleReservation()
{
	static NmAdjRegistry r;
	NmAdjInit(&r);
	NmAdjAdd(&r, E(5, 6), 900, NMADJ_CLAIMED, 0, 0);
	NmJobDesc h = E(5, 5), g = E(5, 7);
	NmAdjBgScan bs;
	NmAdjBgResult br;
	NmAdjBgScanBegin(&bs, &r, true, 0);
	NmAdjBgScanOffer(&bs, &r, 100, &h, true, false);
	NmAdjBgScanEnd(&bs, &r, &br);
	Check(br.newReservation, "H reserved");
	// H is gone; G (also blocked) is the only job.
	NmAdjBgScanBegin(&bs, &r, true, 1);
	NmAdjBgScanOffer(&bs, &r, 101, &g, true, false);
	NmAdjBgScanEnd(&bs, &r, &br);
	int res = NmAdjFindOwned(&r, NMADJ_BG, NMADJ_RESERVED);
	Check(br.droppedRes && br.decision == NMADJ_BG_WAIT && res >= 0 && r.e[res].task == 101,
	      "the lapsed reservation passes to the next blocked job");
}

int main()
{
	TestCellOf();
	TestPredicate();
	TestRegistry();
	TestSimMixed();
	TestSimCountMode();
	TestSimStarvation();
	TestSimTwoReservations();
	TestSimStaleReservation();
	TestSimNoWorkers();
	TestSimDrainStall();
	TestWorkerTakesBgReservation();
	TestBgStaleReservation();
	TestBgSkipKeepsConflictOrder();
	TestBusyWorkersBgOnlySkip();
	TestBgOnlyAgeCanFail();
	TestBgOnlyAgeAfterIdle();
	TestBgReservationVsYoungerAge();
	TestSimSlowCell();
	return CheckExit("nm_adjacency_units");
}
