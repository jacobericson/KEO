#include <cstdio>
#include "movement/k7_swap_policy.h"

#include "check.h"

static const double MARGIN   = 0.5;
static const double HOLD_MAX = 60.0;

int main()
{
	// -------------------------------------------------------------------
	// K7SigOnsetStep: the latch itself.
	// -------------------------------------------------------------------
	{
		double onset = 0.0;
		onset = K7SigOnsetStep(onset, false, false, 100.0);
		Check(onset == 0.0, "onset stays 0 with no signature");
		onset = K7SigOnsetStep(onset, true, false, 111.8);
		Check(onset == 111.8, "onset arms on the first signature frame");
		// The design's must-HOLD case: the signature holds every frame from
		// 111.8 to 115.0 (a stopped character keeps hc136==1 or ps==3 every
		// frame), so the "last frame it held" value would read ~115.0 -- but
		// the onset must stay pinned at its first frame.
		for (double t = 112.0; t <= 115.0; t += 0.25)
			onset = K7SigOnsetStep(onset, true, false, t);
		Check(onset == 111.8, "onset never drifts to the last-frame value");
		onset = K7SigOnsetStep(onset, false, true, 116.0);
		Check(onset == 0.0, "a task-29 poll clears the onset");
		onset = K7SigOnsetStep(onset, false, false, 117.0);
		Check(onset == 0.0, "the signature stopping (no task-29) does not re-arm it");
	}

	// -------------------------------------------------------------------
	// K7ClassifySwap: the doc's own worked cases (stop-fixes-design.md 2.5).
	// -------------------------------------------------------------------

	// The case that must HOLD: onset 111.8, swap first seen 115.1, cur=6
	// (EQUIP_WEAPON, combat), no deletion latched yet, well under holdMax.
	Check(K7ClassifySwap(/*last29*/111.5, /*sig*/111.8, /*deletedSince*/0.0,
	                     /*swapSeen*/115.1, /*now*/115.1, /*curType*/6,
	                     MARGIN, HOLD_MAX) == K7_SWAP_HOLD,
	      "r5 as recorded: died first, combat task -> HOLD");

	// Same signature, but the new task is not on the combat whitelist.
	Check(K7ClassifySwap(111.5, 111.8, 0.0, 115.1, 115.1, /*curType*/26,
	                     MARGIN, HOLD_MAX) == K7_SWAP_DROP_NONCOMBAT,
	      "died first, non-combat task -> DROP_NONCOMBAT");

	// A ladder rung failure predates the last task-29 poll: the order lived
	// on past it, so a later swap is not "died first".
	Check(K7ClassifySwap(/*last29*/114.9, /*sig*/110.0, 0.0,
	                     /*swapSeen*/115.1, 115.1, 6, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
	      "rung failure before the last task-29 poll -> DROP");

	// The signature and the swap are seen in (almost) the same poll: an
	// AI-driven switch in the same tick the order ends, not a died-first swap.
	Check(K7ClassifySwap(/*last29*/109.0, /*sig*/115.0, 0.0,
	                     /*swapSeen*/115.1, 115.1, 6, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
	      "swap seen within the margin of the signature -> DROP");

	// A deletion is already latched (a hold's second-or-later poll) and the
	// signature falls inside K7_SIG_WINDOW of it: still HOLD, independent of
	// last29/swapSeen ordering (the entry is already past its first poll).
	Check(K7ClassifySwap(/*last29*/50.0, /*sig*/111.8, /*deletedSince*/113.0,
	                     /*swapSeen*/50.5, /*now*/120.0, 6, MARGIN, HOLD_MAX) == K7_SWAP_HOLD,
	      "deletion latched, signature inside K7_SIG_WINDOW -> HOLD");

	// The hold has run past holdMax since it was latched.
	Check(K7ClassifySwap(50.0, 111.8, /*deletedSince*/60.0, 50.5, /*now*/121.0,
	                     6, MARGIN, HOLD_MAX) == K7_SWAP_EXPIRED,
	      "now - deletedSince = 61 > holdMax -> EXPIRED");

	// No signature at all: never died first.
	Check(K7ClassifySwap(109.0, 0.0, 0.0, 115.1, 115.1, 6, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
	      "sig=0 -> DROP");

	// -------------------------------------------------------------------
	// K7IsCombatTask whitelist spot checks.
	// -------------------------------------------------------------------
	Check(K7IsCombatTask(6), "EQUIP_WEAPON (6) is combat");
	Check(K7IsCombatTask(4), "MELEE_ATTACK (4) is combat");
	Check(K7IsCombatTask(32), "SELF_PRESERVATION (32) is combat");
	Check(K7IsCombatTask(35), "RUN_AWAY (35) is combat");
	Check(K7IsCombatTask(62), "STAND_STILL (62) is combat");
	Check(K7IsCombatTask(147), "the forced stumble (147) is combat");
	Check(!K7IsCombatTask(26), "an arbitrary job task (26) is not combat");
	Check(!K7IsCombatTask(29), "ORDER_TYPE_MOVE itself is never on the combat list");
	Check(!K7IsCombatTask(-1), "-1 (no task) is not combat");

	// -------------------------------------------------------------------
	// Regression families (stop-fixes-design.md 2.5): every one that must
	// still DROP after Fix A.
	// -------------------------------------------------------------------

	// Live move interrupted by combat, no death first (sig=0 entirely).
	Check(K7ClassifySwap(100.0, 0.0, 0.0, 100.2, 100.2, 6, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
	      "family: live move interrupted by combat, no prior signature -> DROP");

	// Order died, then idle job or medic care (non-combat) -- xo.
	Check(K7ClassifySwap(90.0, 95.0, 0.0, 96.0, 96.0, /*medic-ish*/50, MARGIN, HOLD_MAX)
	      == K7_SWAP_DROP_NONCOMBAT,
	      "family: died then a non-combat task -> DROP_NONCOMBAT (xo)");

	// Order died, fight longer than 60s -- xe once EXPIRED is reached, but
	// still HOLD before that (a genuine player task swap during combat must
	// keep waiting, not drop early).
	Check(K7ClassifySwap(90.0, 95.0, /*deletedSince*/95.0, 96.0, /*now*/150.0,
	                     6, MARGIN, HOLD_MAX) == K7_SWAP_HOLD,
	      "family: fight still running at 55s -> still HOLD");
	Check(K7ClassifySwap(90.0, 95.0, 95.0, 96.0, /*now*/156.0,
	                     6, MARGIN, HOLD_MAX) == K7_SWAP_EXPIRED,
	      "family: fight running past 60s -> EXPIRED (xe)");

	// A genuine player task swap (no death first) must still drop even
	// though the new task is combat: this is the case the margin/last29
	// ordering exists to keep out of HOLD.
	Check(K7ClassifySwap(/*last29*/120.0, /*sig*/0.0, 0.0, /*swapSeen*/120.1, 120.1,
	                     6, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
	      "family: a genuine player-driven combat swap with no prior death -> DROP");

	// -------------------------------------------------------------------
	// K7DestReadyAllows (Fix B3).
	// -------------------------------------------------------------------
	Check(K7DestReadyAllows(2 /* ZR_BUILDINGS_PENDING */, 0.0, 15.0),
	      "the outdoor instance in the world always allows");
	Check(!K7DestReadyAllows(3 /* ZR_NOT_IN_WORLD */, 0.0, 15.0),
	      "not in the world, freshly refused -> refuse");
	Check(!K7DestReadyAllows(4 /* ZR_UNKNOWN */, 14.9, 15.0),
	      "unknown, just under the wait cap -> still refuse");
	Check(K7DestReadyAllows(3 /* ZR_NOT_IN_WORLD */, 15.0, 15.0),
	      "not in the world for exactly the wait cap -> force allow");
	Check(K7DestReadyAllows(4 /* ZR_UNKNOWN */, 20.0, 15.0),
	      "past the wait cap -> force allow regardless of class");

	// -------------------------------------------------------------------
	// Pin B1's invariant: K7RebasePausedClocks (k7_reissue.cpp) must
	// shift every timestamp a K7 comparison reads together, never one alone.
	// A uniform shift of every input by the same delta must never change the
	// verdict; shifting deletedSince alone (the partial-rebase regression)
	// must.
	// -------------------------------------------------------------------
	{
		double l29 = 5.0, sig = 10.0, del = 11.0, seen = 10.3, now = 12.0, d = 5.0;
		int combat = 6;   // EQUIP_WEAPON
		Check(K7ClassifySwap(l29, sig, del, seen, now, combat, MARGIN, HOLD_MAX)
		   == K7ClassifySwap(l29 + d, sig + d, del + d, seen + d, now + d, combat, MARGIN, HOLD_MAX),
		      "a uniform shift keeps the verdict");
		Check(K7ClassifySwap(l29, sig, del, seen, now, combat, MARGIN, HOLD_MAX) == K7_SWAP_HOLD
		   && K7ClassifySwap(l29, sig, del + d, seen, now + d, combat, MARGIN, HOLD_MAX) == K7_SWAP_DROP,
		      "shifting deletedSince alone flips HOLD to DROP (the partial-rebase regression)");
	}

	return CheckExit("k7_swap_units");
}
