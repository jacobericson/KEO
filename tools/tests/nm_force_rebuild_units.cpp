// The rebuild-navmesh key's pure rules: the mark word, the claim, press,
// finish and front decisions, the cache bypass, the neighbour selection, the
// eligibility and caller tests, and the panel hold.

#include <cstdio>
#include <cstring>
#include "navmesh/cache/nm_force_rebuild_policy.h"

#include "check.h"

// A world coordinate in cell c at fraction f of a 4608-unit cell.
static float At(int c, float f) { return ((float)c + f - 32.0f) * 4608.0f; }

static void TestWord()
{
	Check(NmMarkStateOf(NmMarkWord(7, NM_MARK_CLAIMED)) == NM_MARK_CLAIMED && NmMarkSeqOf(NmMarkWord(7, NM_MARK_CLAIMED)) == 7,
	      "a word keeps its state and sequence");
	Check(NmMarkWord(NM_MARK_SEQ_MAX, NM_MARK_DONE) > 0, "the largest sequence's word stays positive");
	Check(NmMarkNextSeq(0) == 1 && NmMarkNextSeq(5) == 6 && NmMarkNextSeq(NM_MARK_SEQ_MAX) == 1,
	      "sequences run 1..max and wrap past 0");
	Check(NmMarkStateOf(0) == NM_MARK_NONE && NmMarkSeqOf(0) == 0, "a zero word is no mark");
}

static void TestClaim()
{
	const long marked = NmMarkWord(3, NM_MARK_MARKED);
	Check(NmMarkClaimDecide(marked, 1.0, 0) == NM_CLAIM_CONSUME, "a type-0 claim consumes a live mark");
	Check(NmMarkClaimDecide(marked, 1.0, 1) == NM_CLAIM_NONE, "a type-1 job never consumes a mark");
	Check(NmMarkClaimDecide(marked, 1.0, 3) == NM_CLAIM_NONE, "an interior job never consumes a mark");
	Check(NmMarkClaimDecide(marked, NM_REBUILD_MARK_TTL_SEC, 0) == NM_CLAIM_CONSUME &&
	      NmMarkClaimDecide(marked, NM_REBUILD_MARK_TTL_SEC + 0.001, 0) == NM_CLAIM_EXPIRED,
	      "a mark expires just past its TTL");
	Check(NmMarkClaimDecide(NmMarkWord(3, NM_MARK_CLAIMED), 1.0, 0) == NM_CLAIM_NONE &&
	      NmMarkClaimDecide(NmMarkWord(3, NM_MARK_DONE), 1.0, 0) == NM_CLAIM_NONE &&
	      NmMarkClaimDecide(0, 1.0, 0) == NM_CLAIM_NONE,
	      "only a MARKED mark is consumed, once");
}

static void TestPress()
{
	Check(NmMarkPressDecide(NmMarkWord(4, NM_MARK_CLAIMED), 5.0) == NM_PRESS_KEEP,
	      "a press keeps a mark a job is working on");
	Check(NmMarkPressDecide(NmMarkWord(4, NM_MARK_CLAIMED), NM_REBUILD_MARK_TTL_SEC + 1.0) == NM_PRESS_MARK,
	      "a press re-marks a claim older than the TTL");
	Check(NmMarkPressDecide(NmMarkWord(4, NM_MARK_MARKED), 5.0) == NM_PRESS_MARK &&
	      NmMarkPressDecide(NmMarkWord(4, NM_MARK_DONE), 5.0) == NM_PRESS_MARK && NmMarkPressDecide(0, 0.0) == NM_PRESS_MARK,
	      "a press marks anything else afresh");
	const long w = NmMarkWord(9, NM_MARK_MARKED);
	Check(!NmMarkPressFinished(w, w) && !NmMarkPressFinished(w, NmMarkWord(9, NM_MARK_CLAIMED)),
	      "a press's cell is pending while its mark is marked or claimed");
	Check(NmMarkPressFinished(w, NmMarkWord(9, NM_MARK_DONE)) && NmMarkPressFinished(w, NmMarkWord(9, NM_MARK_NONE)) &&
	      NmMarkPressFinished(w, NmMarkWord(10, NM_MARK_MARKED)) && NmMarkPressFinished(w, 0),
	      "a press's cell has ended when its mark is done, cleared or another press's");
	Check(NmMarkCellDecide(NmMarkWord(4, NM_MARK_CLAIMED), 5.0, false) == NM_PRESS_MARK,
	      "cell mark: the border entry re-marks a mark a job holds");
	Check(NmMarkCellDecide(NmMarkWord(4, NM_MARK_CLAIMED), 5.0, true) == NM_PRESS_KEEP,
	      "cell mark: the key's form keeps a mark a job holds");
	bool fresh = true;
	const long others[] = { NmMarkWord(4, NM_MARK_MARKED), NmMarkWord(4, NM_MARK_DONE), 0 };
	for (int keep = 0; keep < 2; ++keep)
	{
		for (int i = 0; i < 3; ++i)
			fresh = fresh && NmMarkCellDecide(others[i], 5.0, keep != 0) == NM_PRESS_MARK;
		fresh = fresh && NmMarkCellDecide(NmMarkWord(4, NM_MARK_CLAIMED), NM_REBUILD_MARK_TTL_SEC + 1.0, keep != 0) == NM_PRESS_MARK;
	}
	Check(fresh, "cell mark: anything else is marked afresh either way");
}

static void TestFrontAndBypass()
{
	const long marked = NmMarkWord(2, NM_MARK_MARKED);
	Check(NmMarkWantsFront(marked, 1.0, 0), "a marked cell's type-0 job goes to the front");
	Check(!NmMarkWantsFront(marked, 1.0, 1) && !NmMarkWantsFront(marked, 1.0, 3),
	      "only type-0 jobs go to the front");
	Check(!NmMarkWantsFront(NmMarkWord(2, NM_MARK_CLAIMED), 1.0, 0) && !NmMarkWantsFront(marked, NM_REBUILD_MARK_TTL_SEC + 1.0, 0),
	      "a claimed or expired mark fronts nothing");
	Check(!NmJobMayReadCache(true, true) && !NmJobMayReadCache(false, true),
	      "a forced job never reads the cache");
	Check(NmJobMayReadCache(true, false) && !NmJobMayReadCache(false, false),
	      "a normal job reads the cache exactly when its key is trusted");
}

// Middle -> 0; each edge -> 1; each corner -> 3; the boundaries; the grid border.
static void TestSelect()
{
	NmRebuildSelection s;
	NmRebuildSelect(At(10, 0.5f), At(20, 0.5f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.matched && s.count == 0 && s.dropped == 0, "the middle of a cell selects no neighbour");
	NmRebuildSelect(At(10, 0.1f), At(20, 0.5f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 1 && s.n[0].gx == 9 && s.n[0].gy == 20, "near the low x edge: the cell before on x");
	NmRebuildSelect(At(10, 0.9f), At(20, 0.5f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 1 && s.n[0].gx == 11 && s.n[0].gy == 20, "near the high x edge: the cell after on x");
	NmRebuildSelect(At(10, 0.5f), At(20, 0.1f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 1 && s.n[0].gx == 10 && s.n[0].gy == 19, "near the low z edge: the cell before on z");
	NmRebuildSelect(At(10, 0.5f), At(20, 0.9f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 1 && s.n[0].gx == 10 && s.n[0].gy == 21, "near the high z edge: the cell after on z");
	NmRebuildSelect(At(10, 0.1f), At(20, 0.1f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 3 && s.n[0].gx == 9 && s.n[0].gy == 20 && s.n[1].gx == 10 && s.n[1].gy == 19 &&
	      s.n[2].gx == 9 && s.n[2].gy == 19, "near the low-low corner: both edges and the diagonal");
	NmRebuildSelect(At(10, 0.9f), At(20, 0.9f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 3 && s.n[2].gx == 11 && s.n[2].gy == 21, "near the high-high corner");
	NmRebuildSelect(At(10, 0.1f), At(20, 0.9f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 3 && s.n[2].gx == 9 && s.n[2].gy == 21, "near the low-high corner");
	NmRebuildSelect(At(10, 0.9f), At(20, 0.1f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 3 && s.n[2].gx == 11 && s.n[2].gy == 19, "near the high-low corner");

	NmRebuildSelect(At(10, 0.25f), At(20, 0.75f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.matched && s.count == 0, "exactly the edge fraction from a side is not near it");
	NmRebuildSelect(At(10, 0.2499f), At(20, 0.7501f), 4608.0f, 4608.0f, 10, 20, &s);
	Check(s.count == 3, "just inside the edge fraction is near");

	NmRebuildSelect(At(0, 0.1f), At(20, 0.5f), 4608.0f, 4608.0f, 0, 20, &s);
	Check(s.count == 0 && s.dropped == 1, "at the grid's low x border the outside cell is dropped");
	NmRebuildSelect(At(63, 0.9f), At(63, 0.9f), 4608.0f, 4608.0f, 63, 63, &s);
	Check(s.count == 0 && s.dropped == 3, "at the grid's far corner all three outside cells are dropped");
	NmRebuildSelect(At(0, 0.1f), At(63, 0.5f), 4608.0f, 4608.0f, 0, 63, &s);
	Check(s.count == 0 && s.dropped == 1, "a border cell's edge neighbour outside the grid is dropped");
	NmRebuildSelect(At(0, 0.9f), At(0, 0.1f), 4608.0f, 4608.0f, 0, 0, &s);
	Check(s.count == 1 && s.n[0].gx == 1 && s.n[0].gy == 0 && s.dropped == 2, "a grid corner keeps its inside neighbour");

	NmRebuildSelect(At(10, 0.1f), At(20, 0.1f), 4608.0f, 4608.0f, 11, 20, &s);
	Check(!s.matched && s.count == 0, "a cell other than the game's selects nothing");
	NmRebuildSelect(At(10, 0.1f), At(20, 0.1f), 0.0f, 4608.0f, 10, 20, &s);
	Check(!s.matched && s.count == 0, "no cell size selects nothing");
	NmRebuildSelect(At(70, 0.5f), At(20, 0.5f), 4608.0f, 4608.0f, 63, 20, &s);
	Check(!s.matched && s.count == 0, "a point off the grid selects nothing");
}

static void TestEligibleAndCaller()
{
	Check(NmRebuildEligible(true, true, false, true, true) == NM_SKIP_NONE, "an accessible loaded cell is eligible");
	Check(NmRebuildEligible(false, true, false, true, true) == NM_SKIP_NO_ZONE &&
	      NmRebuildEligible(true, false, true, true, true) == NM_SKIP_PRIVATE &&
	      NmRebuildEligible(true, false, false, true, true) == NM_SKIP_NOT_ACCESSIBLE &&
	      NmRebuildEligible(true, true, false, false, true) == NM_SKIP_NO_CONTENT &&
	      NmRebuildEligible(true, true, false, true, false) == NM_SKIP_NO_TERRAIN,
	      "each missing condition names its skip");
	Check(strcmp(NmRebuildSkipName(NM_SKIP_PRIVATE), "private") == 0 && strcmp(NmRebuildSkipName(99), "?") == 0,
	      "skip names");
	const unsigned __int64 key = 0x787CF9;
	unsigned __int64 f[3] = { 0x787CF9, 1, 2 };
	Check(NmRebuildIsKeyCaller(f, 1, key), "the key's own return address");
	f[0] = 0x55555; f[2] = 0x787CF9;
	Check(NmRebuildIsKeyCaller(f, 3, key), "the key's return one detour further down");
	Check(!NmRebuildIsKeyCaller(f, 2, key) && !NmRebuildIsKeyCaller(f, 0, key), "no frame matches: an editor call");
}

static void TestHold()
{
	Check(NmHoldSuppresses(true, false, 1.0), "an off-main dismissal under the cap is held");
	Check(!NmHoldSuppresses(true, true, 1.0), "a main-thread dismissal passes: it ends a real load");
	Check(!NmHoldSuppresses(true, false, NM_REBUILD_HOLD_CAP_SEC) && !NmHoldSuppresses(true, false, NM_REBUILD_HOLD_CAP_SEC + 5.0),
	      "past the cap the path thread's dismissal passes");
	Check(!NmHoldSuppresses(false, false, 1.0), "no hold, no suppression");
	Check(NmHoldDecide(false, 3, 1.0) == NM_HOLD_IDLE && NmHoldDecide(true, 0, 1.0) == NM_HOLD_DONE &&
	      NmHoldDecide(true, 2, 1.0) == NM_HOLD_PENDING && NmHoldDecide(true, 2, NM_REBUILD_HOLD_CAP_SEC) == NM_HOLD_CAPPED,
	      "the hold's verdicts");
	Check(NmHoldReplayNow(true, true, 0, false), "an owed dismissal replays with the zone manager idle");
	Check(!NmHoldReplayNow(true, true, 1, false) && !NmHoldReplayNow(true, true, 0, true) &&
	      !NmHoldReplayNow(true, false, 0, false) && !NmHoldReplayNow(false, true, 0, false),
	      "never during a load, a save load, before a zone manager, or when nothing is owed");
}

// At the hold's end the panel a press held or showed is dismissed; done and
// capped behave alike, and nothing is owed while the hold is still pending.
static void TestRelease()
{
	Check(NmHoldReleaseOwes(NM_HOLD_DONE, true, false), "a held dismissal is replayed when the hold is done");
	Check(NmHoldReleaseOwes(NM_HOLD_DONE, false, true),
	      "the press's show with no dismissal since is dismissed when the hold is done");
	Check(!NmHoldReleaseOwes(NM_HOLD_DONE, false, false), "neither held nor shown: nothing is replayed when the hold is done");
	Check(NmHoldReleaseOwes(NM_HOLD_DONE, true, true), "held and shown: one dismissal is owed when the hold is done");
	Check(NmHoldReleaseOwes(NM_HOLD_CAPPED, true, false), "a held dismissal is replayed at the cap");
	Check(NmHoldReleaseOwes(NM_HOLD_CAPPED, false, true),
	      "the press's show with no dismissal since is dismissed at the cap");
	Check(!NmHoldReleaseOwes(NM_HOLD_CAPPED, false, false),
	      "neither held nor shown, or shown and dismissed since: nothing is replayed at the cap");
	Check(!NmHoldReleaseOwes(NM_HOLD_PENDING, true, true) && !NmHoldReleaseOwes(NM_HOLD_IDLE, true, true),
	      "a pending or idle hold owes nothing at its end");
}

// One step of a press-show sequence: a show inside the key's call with the
// hold armed ('S'), a show outside it ('s'), or a dismissal that reached the
// game ('D').
static long PressShowStep(long cur, char step)
{
	if (step == 'S')
		return NmPressShowNext(cur, NM_PRESS_SHOW, true);
	if (step == 's')
		return NmPressShowNext(cur, NM_PRESS_SHOW, false);
	return NmPressShowNext(cur, NM_PRESS_DISMISSED, false);
}

// Runs the steps from a cleared flag, then the hold's release (done, nothing
// held): whether a dismissal is owed, and the flag the release leaves.
static bool OwesAfter(const char* steps, long* afterRelease)
{
	long cur = 0;
	for (const char* p = steps; *p; ++p)
		cur = PressShowStep(cur, *p);
	const long old = cur;
	*afterRelease  = NmPressShowNext(cur, NM_PRESS_RELEASE, false);
	return NmHoldReleaseOwes(NM_HOLD_DONE, false, old != 0);
}

static void TestPressShowSequences()
{
	long after = -1;
	Check(OwesAfter("S", &after) && after == 0, "the key's show with the hold armed, then the release: one dismissal owed");
	Check(!OwesAfter("SD", &after) && after == 0,
	      "the key's show, then a dismissal that reached the game, then the release: nothing owed");
	Check(!OwesAfter("s", &after) && after == 0,
	      "a show outside the key's call or with no hold, then the release: nothing owed");
	Check(!OwesAfter("", &after) && !OwesAfter("D", &after), "no show, then the release: nothing owed");
	Check(OwesAfter("SDS", &after) && after == 0,
	      "a show, a dismissal and a second show, then the release: one dismissal owed");
	Check(OwesAfter("Ss", &after), "a later show outside the key's call keeps the flag the key's show set");
	Check(NmPressShowNext(1, NM_PRESS_RELEASE, true) == 0 && NmPressShowNext(0, NM_PRESS_RELEASE, false) == 0,
	      "the release always clears the flag");
}

int main()
{
	TestWord();
	TestClaim();
	TestPress();
	TestFrontAndBypass();
	TestSelect();
	TestEligibleAndCaller();
	TestHold();
	TestRelease();
	TestPressShowSequences();
	return CheckExit("nm_force_rebuild_units");
}
