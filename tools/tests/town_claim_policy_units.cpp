// Host tests for the player-placement town fix: the decision, the containing-town pick, and
// the bytes of the stub, its vanilla gate and the patched site.

#include <cstdio>
#include <cstring>
#include "fixes/world/town_claim_policy.h"

#include "check.h"

static int Rel32At(const unsigned char* p) { int d = 0; memcpy(&d, p, 4); return d; }

// An unparented player placement inside the builder, arriving with a player town.
static TownClaimInputs Placement()
{
	TownClaimInputs in;
	memset(&in, 0, sizeof(in));
	in.depthSet = true;
	in.ownerIsPlayer = true;
	in.townIsPlayerTown = true;
	return in;
}

static TownClaimInputs NullTown(bool containing)
{
	TownClaimInputs in = Placement();
	in.townIsNull = true;
	in.townIsPlayerTown = false;
	in.haveContainingPlayerTown = containing;
	return in;
}

static TownClaimInputs FromBits(unsigned bits)
{
	TownClaimInputs in;
	in.depthSet                 = (bits & (1u << 0)) != 0;
	in.ownerIsPlayer            = (bits & (1u << 1)) != 0;
	in.isFoliage                = (bits & (1u << 2)) != 0;
	in.hasFurnitureOf           = (bits & (1u << 3)) != 0;
	in.hasDoorOf                = (bits & (1u << 4)) != 0;
	in.hasIndoorsOf             = (bits & (1u << 5)) != 0;
	in.hasSaveState             = (bits & (1u << 6)) != 0;
	in.townIsNull               = (bits & (1u << 7)) != 0;
	in.townIsPlayerTown         = (bits & (1u << 8)) != 0;
	in.hasSnapTarget            = (bits & (1u << 9)) != 0;
	in.snapTargetIsPlayerOwned  = (bits & (1u << 10)) != 0;
	in.haveContainingPlayerTown = (bits & (1u << 11)) != 0;
	in.spotInNpcTown            = (bits & (1u << 12)) != 0;
	in.townHoldsSpot            = (bits & (1u << 13)) != 0;
	in.createsPlayerTown        = (bits & (1u << 14)) != 0;
	return in;
}
static const unsigned kAllBits = 1u << 15;

// The rule before the NPC-town case, written out.
static TownClaimAction BaseRule(const TownClaimInputs& in)
{
	return TownClaimNeedsTown(in) ? (in.haveContainingPlayerTown ? TC_USE_CONTAINING : TC_USE_NULL_TOWN)
	                              : TC_PASS;
}

// The case in which the game's choice stands, written out from its inputs.
static bool IsNpcCase(const TownClaimInputs& in)
{
	return TownClaimFlagged(in) && in.spotInNpcTown && !in.hasSnapTarget
	    && in.haveContainingPlayerTown && !in.createsPlayerTown
	    && !(in.townIsPlayerTown && in.townHoldsSpot);
}

static void CheckDecision()
{
	{
		TownClaimInputs in = NullTown(true);
		in.depthSet = false;
		Check(TownClaimDecide(in) == TC_PASS && !TownClaimFlagged(in),
		      "decide: outside the builder passes");
	}
	{
		TownClaimInputs in = NullTown(true);
		in.ownerIsPlayer = false;
		Check(TownClaimDecide(in) == TC_PASS && !TownClaimFlagged(in),
		      "decide: an NPC owner passes");
	}
	{
		TownClaimInputs a = NullTown(true); a.hasFurnitureOf = true;
		TownClaimInputs b = NullTown(true); b.hasDoorOf = true;
		TownClaimInputs c = NullTown(true); c.hasIndoorsOf = true;
		Check(TownClaimDecide(a) == TC_PASS && !TownClaimFlagged(a)
		   && TownClaimDecide(b) == TC_PASS && !TownClaimFlagged(b)
		   && TownClaimDecide(c) == TC_PASS && !TownClaimFlagged(c),
		      "decide: a parented part passes");
	}
	{
		TownClaimInputs in = NullTown(true);
		in.hasSaveState = true;
		Check(TownClaimDecide(in) == TC_PASS && !TownClaimFlagged(in),
		      "decide: a saved state passes");
	}
	{
		TownClaimInputs in = NullTown(true);
		in.isFoliage = true;
		Check(TownClaimDecide(in) == TC_PASS && !TownClaimFlagged(in),
		      "decide: foliage passes");
	}
	{
		TownClaimInputs in = Placement();
		TownClaimInputs snapped = Placement();
		snapped.hasSnapTarget = true;
		Check(TownClaimDecide(in) == TC_PASS && TownClaimFlagged(in)
		   && TownClaimDecide(snapped) == TC_PASS && TownClaimFlagged(snapped),
		      "decide: a player town passes");
	}
	Check(TownClaimDecide(NullTown(true)) == TC_USE_CONTAINING,
	      "decide: a null town becomes the containing town");
	Check(TownClaimDecide(NullTown(false)) == TC_USE_NULL_TOWN,
	      "decide: a null town without a containing town becomes noTown");
	{
		TownClaimInputs in = Placement();
		in.townIsPlayerTown = false;
		in.hasSnapTarget = true;
		TownClaimInputs covered = in;
		covered.haveContainingPlayerTown = true;
		Check(TownClaimNeedsTown(in) && TownClaimDecide(in) == TC_USE_NULL_TOWN
		   && TownClaimDecide(covered) == TC_USE_CONTAINING,
		      "decide: an NPC town from an NPC-owned snap target is replaced");
	}
	{
		TownClaimInputs in = Placement();
		in.townIsPlayerTown = false;
		in.hasSnapTarget = true;
		in.snapTargetIsPlayerOwned = true;
		in.haveContainingPlayerTown = true;
		Check(!TownClaimNeedsTown(in) && TownClaimDecide(in) == TC_PASS && TownClaimFlagged(in),
		      "decide: a player-owned snap target in an NPC town keeps vanilla's town");
	}
	{
		TownClaimInputs in = Placement();
		in.townIsPlayerTown = false;
		in.haveContainingPlayerTown = true;
		Check(!TownClaimNeedsTown(in) && TownClaimDecide(in) == TC_PASS,
		      "decide: an NPC town without a snap target is kept");
	}

	bool neverNull = true;
	int nullFlagged = 0;
	for (unsigned bits = 0; bits < kAllBits; ++bits)
	{
		const TownClaimInputs in = FromBits(bits);
		if (!TownClaimFlagged(in) || !in.townIsNull)
			continue;
		const TownClaimAction a = TownClaimDecide(in);
		if (a == TC_VANILLA)
			continue;
		++nullFlagged;
		if (a == TC_PASS
		 || (a == TC_USE_CONTAINING && !in.haveContainingPlayerTown)
		 || (a == TC_USE_NULL_TOWN && in.haveContainingPlayerTown))
			neverNull = false;
	}
	Check(neverNull && nullFlagged > 0, "flagged never leaves t null");

	bool allFlagged = true;
	int placements = 0;
	for (unsigned bits = 0; bits < kAllBits; ++bits)
	{
		const TownClaimInputs in = FromBits(bits);
		if (!in.depthSet || !in.ownerIsPlayer || in.isFoliage || in.hasFurnitureOf
		 || in.hasDoorOf || in.hasIndoorsOf || in.hasSaveState)
			continue;
		++placements;
		if (!TownClaimFlagged(in))
			allFlagged = false;
	}
	Check(allFlagged && placements == 256,
	      "flagged: every unparented player placement is flagged, whatever its town");
}

static void CheckNpcTown()
{
	{
		TownClaimInputs a = NullTown(true);
		a.spotInNpcTown = true;
		Check(TownClaimVanillaStands(a) && TownClaimDecide(a) == TC_VANILLA,
		      "npc town: a null arrival under an owner's town keeps the game's town");
	}
	{
		TownClaimInputs b = Placement();
		b.spotInNpcTown = true;
		b.haveContainingPlayerTown = true;
		b.townHoldsSpot = false;
		TownClaimInputs held = b;
		held.townHoldsSpot = true;
		Check(TownClaimDecide(b) == TC_VANILLA && TownClaimDecide(held) == TC_PASS,
		      "npc town: a player town whose radius misses the spot keeps the game's town");
	}

	int cases = 0, vanilla = 0, insideOther = 0, outside = 0;
	bool caseOk = true, insideOk = true, outsideOk = true, snapOk = true, unflaggedOk = true;
	bool createsOk = true, holdsOk = true, uncoveredOk = true;
	for (unsigned bits = 0; bits < kAllBits; ++bits)
	{
		const TownClaimInputs in = FromBits(bits);
		const TownClaimAction a = TownClaimDecide(in);
		if (a == TC_VANILLA)
			++vanilla;
		if (IsNpcCase(in))
		{
			++cases;
			if (a != TC_VANILLA)
				caseOk = false;
		}
		else if (in.spotInNpcTown)
		{
			++insideOther;
			if (a != BaseRule(in))
				insideOk = false;
		}
		if (!in.spotInNpcTown)
		{
			++outside;
			if (a != BaseRule(in))
				outsideOk = false;
		}
		if (in.hasSnapTarget && (a == TC_VANILLA || a != BaseRule(in)))
			snapOk = false;
		if (!TownClaimFlagged(in) && a == TC_VANILLA)
			unflaggedOk = false;
		if (in.createsPlayerTown && a == TC_VANILLA)
			createsOk = false;
		if (in.townIsPlayerTown && in.townHoldsSpot && a == TC_VANILLA)
			holdsOk = false;
		if (!in.haveContainingPlayerTown && a == TC_VANILLA)
			uncoveredOk = false;
	}
	Check(caseOk && cases > 0, "npc town: the approved case keeps the game's town");
	Check(insideOk && insideOther > 0,
	      "npc town: inside an NPC town, outside the case, the rule is unchanged");
	Check(snapOk, "npc town: a snap keeps the fix");
	Check(unflaggedOk, "npc town: an unflagged call is never TC_VANILLA");
	Check(outsideOk && outside > 0, "npc town: outside every NPC town the rule is unchanged");
	Check(createsOk, "npc town: a building that creates a player town keeps the fix");
	Check(holdsOk, "npc town: a player town whose radius holds the spot keeps the fix");
	Check(uncoveredOk, "npc town: no owner's town over the spot keeps the fix");
	Check(vanilla > 0 && vanilla == cases, "npc town: the exhaustive walk reaches the rule");
}

static void CheckSkipFlag()
{
	Check(!TownClaimSetsSkipFlag(TC_VANILLA, true, true) && !TownClaimSetsSkipFlag(TC_VANILLA, true, false)
	   && !TownClaimSetsSkipFlag(TC_VANILLA, false, true) && !TownClaimSetsSkipFlag(TC_VANILLA, false, false),
	      "flag: the game's choice never sets the skip flag");
	Check(TownClaimSetsSkipFlag(TC_PASS, true, true) && TownClaimSetsSkipFlag(TC_USE_CONTAINING, true, true)
	   && TownClaimSetsSkipFlag(TC_USE_NULL_TOWN, true, true),
	      "flag: a flagged call with a town sets it");
	const TownClaimAction actions[3] = { TC_PASS, TC_USE_CONTAINING, TC_USE_NULL_TOWN };
	bool never = true;
	for (int i = 0; i < 3; ++i)
	{
		never = never && !TownClaimSetsSkipFlag(actions[i], true, false)
		              && !TownClaimSetsSkipFlag(actions[i], false, true)
		              && !TownClaimSetsSkipFlag(actions[i], false, false);
	}
	Check(never, "flag: no town, or an unflagged call, never sets it");
}

static void CheckPick()
{
	{
		const TownClaimCandidate c[3] = { { 300.0f, 0.0f, 500.0f }, { 50.0f, 0.0f, 500.0f },
		                                  { 0.0f, 120.0f, 500.0f } };
		Check(TownClaimPickContaining(c, 3, 0.0f, 0.0f) == 1,
		      "pick: the nearest covering centre wins");
	}
	{
		const TownClaimCandidate c[2] = { { 10.0f, 0.0f, 5.0f }, { 0.0f, 100.0f, 200.0f } };
		Check(TownClaimPickContaining(c, 2, 0.0f, 0.0f) == 1,
		      "pick: a nearer centre whose radius misses loses");
	}
	{
		// The candidate carries no height: a town centre far above or below the spot covers it
		// by its x and z alone, and the z offset counts as much as the x offset.
		const TownClaimCandidate c[1] = { { 100.0f, 200.0f, 10.0f } };
		Check(TownClaimPickContaining(c, 1, 100.0f, 200.0f) == 0
		   && TownClaimPickContaining(c, 1, 100.0f, 1200.0f) == -1
		   && TownClaimPickContaining(c, 1, 1100.0f, 200.0f) == -1,
		      "pick: the x and z offsets count alike");
	}
	{
		const TownClaimCandidate c[1] = { { 0.0f, 0.0f, 5.0f } };
		Check(TownClaimPickContaining(c, 1, 3.0f, 4.0f) == 0
		   && TownClaimPickContaining(c, 1, 3.0f, 4.001f) == -1,
		      "pick: the radius is inclusive");
	}
	{
		const TownClaimCandidate c[2] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -5.0f } };
		Check(TownClaimPickContaining(c, 2, 0.0f, 0.0f) == -1,
		      "pick: a zero radius covers nothing");
	}
	{
		const TownClaimCandidate c[2] = { { 1000.0f, 0.0f, 50.0f }, { 0.0f, -1000.0f, 50.0f } };
		Check(TownClaimPickContaining(c, 2, 0.0f, 0.0f) == -1
		   && TownClaimPickContaining(c, 0, 0.0f, 0.0f) == -1
		   && TownClaimPickContaining(NULL, 2, 0.0f, 0.0f) == -1,
		      "pick: none covers");
	}
}

static void CheckEligible()
{
	Check(TownClaimTownEligible(true, false, false), "eligible: the owner's town is eligible");
	Check(!TownClaimTownEligible(false, false, false), "eligible: another faction's town is refused");
	Check(!TownClaimTownEligible(true, true, false), "eligible: a nest marker is refused");
	Check(!TownClaimTownEligible(true, false, true), "eligible: a nest is refused");
}

static void CheckSnapLea()
{
	// The builder (0x4D6810) loads SetMountedBuildingCallback's vftable (0x16DE9D0) at +0x523.
	const unsigned __int64 base = 0x140000000ULL;
	const unsigned __int64 at = base + 0x4D6810 + kTownClaimSnapLeaOffset;
	const unsigned __int64 vft = base + 0x16DE9D0;
	Check(kTownClaimSnapLeaOffset == 0x523 && kTownClaimSnapLeaLen == 7
	   && TownClaimLeaReaches(kTownClaimSnapLeaBytes, at, vft),
	      "snap lea: the shipped bytes reach the vftable");
	unsigned char b[7];
	memcpy(b, kTownClaimSnapLeaBytes, sizeof(b));
	b[4] ^= 0x01;
	Check(!TownClaimLeaReaches(b, at, vft), "snap lea: a changed disp byte does not reach it");
	memcpy(b, kTownClaimSnapLeaBytes, sizeof(b));
	b[2] = 0x0D;   // lea rcx
	Check(!TownClaimLeaReaches(b, at, vft) && !TownClaimLeaReaches(NULL, at, vft),
	      "snap lea: another instruction is refused");
}

static void CheckBytes()
{
	const unsigned __int64 stub   = 0x7FF600000000ULL;
	const unsigned __int64 slot   = stub - 0x1000;
	const unsigned __int64 keep   = 0x7FF5C057CC7EULL;
	const unsigned __int64 resume = 0x7FF5C057C9BEULL;
	const unsigned __int64 site   = 0x7FF5C057C9B1ULL;

	unsigned char s[0x80];
	memset(s, 0xCC, sizeof(s));
	size_t len = 0;
	const bool built = BuildTownClaimStub(s, sizeof(s), stub, slot, keep, resume, &len);

	Check(built && memcmp(s, kTownClaimSiteBytes, 7) == 0,
	      "stub: the replayed compare is the site's first seven bytes");
	Check(built && s[0x07] == 0x0F && s[0x08] == 0x84
	   && Rel32At(s + 0x09) == (int)((__int64)keep - (__int64)(stub + 0x0D)),
	      "stub: je lands on the keep label");
	Check(built && s[0x1C] == 0xFF && s[0x1D] == 0x15
	   && Rel32At(s + 0x1E) == (int)((__int64)slot - (__int64)(stub + 0x22)),
	      "stub: the gate call reads its slot");
	{
		static const unsigned char kPush[11] =
			{ 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 };
		static const unsigned char kPop[11] =
			{ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 };
		static const unsigned char kSub[4] = { 0x48, 0x83, 0xEC, 0x28 };
		static const unsigned char kAdd[6] = { 0x48, 0x83, 0xC4, 0x28, 0x85, 0xC0 };
		Check(built && memcmp(s + 0x0D, kPush, 11) == 0 && memcmp(s + 0x18, kSub, 4) == 0
		   && memcmp(s + 0x22, kAdd, 6) == 0 && memcmp(s + 0x28, kPop, 11) == 0,
		      "stub: seven pushes and seven pops in reverse");
	}
	Check(built && s[0x33] == 0x0F && s[0x34] == 0x85
	   && Rel32At(s + 0x35) == (int)((__int64)keep - (__int64)(stub + 0x39)),
	      "stub: the skip branch lands on the keep label");
	Check(built && s[0x39] == 0xE9
	   && Rel32At(s + 0x3A) == (int)((__int64)resume - (__int64)(stub + 0x3E)),
	      "stub: the fall-through jumps to the resume");
	{
		unsigned char small[0x3D];
		Check(built && len == 0x3E && kTownClaimStubLen == 0x3E && s[0x3E] == 0xCC
		   && !BuildTownClaimStub(small, sizeof(small), stub, slot, keep, resume, NULL),
		      "stub: length 0x3E");
	}
	{
		unsigned char t[0x80];
		Check(!BuildTownClaimStub(t, sizeof(t), stub, stub + 0x100000000ULL, keep, resume, NULL)
		   && !BuildTownClaimStub(t, sizeof(t), stub, stub - 0x100000000ULL, keep, resume, NULL),
		      "stub: an unreachable gate slot is refused");
	}

	{
		unsigned char p[13];
		memset(p, 0, sizeof(p));
		const bool ok = BuildTownClaimSitePatch(kTownClaimSiteBytes, site, stub, p);
		bool nops = true;
		for (int i = 5; i < 13; ++i)
			nops = nops && p[i] == 0x90;
		Check(ok && p[0] == 0xE9 && Rel32At(p + 1) == (int)((__int64)stub - (__int64)(site + 5))
		   && nops,
		      "site: a jump to the stub and eight nops");
	}
	{
		unsigned char cur[13];
		memcpy(cur, kTownClaimSiteBytes, sizeof(cur));
		cur[12] = 0x01;
		unsigned char p[13];
		Check(!BuildTownClaimSitePatch(cur, site, stub, p)
		   && !BuildTownClaimSitePatch(kTownClaimSiteBytes, site, site + 0x100000000ULL, p)
		   && !BuildTownClaimSitePatch(NULL, site, stub, p),
		      "site: other bytes are refused");
	}
	{
		// The jz in the thirteen bytes, taken from the resume address, reaches the keep label:
		// the two neighbours and the site's own displacement agree.
		Check(kTownClaimResumeRva - kTownClaimSiteRva == (unsigned __int64)kTownClaimSiteLen
		   && kTownClaimSiteRva - kTownClaimLeadRva == (unsigned __int64)kTownClaimLeadLen
		   && kTownClaimResumeRva + (unsigned __int64)Rel32At(kTownClaimSiteBytes + 9)
		      == kTownClaimKeepRva,
		      "site: the original jz reaches the keep label");
	}

	{
		unsigned char g[4] = { 0xCC, 0xCC, 0xCC, 0xCC };
		unsigned char tiny[2];
		Check(BuildTownClaimVanillaGate(g, sizeof(g)) && g[0] == 0x33 && g[1] == 0xC0
		   && g[2] == 0xC3 && g[3] == 0xCC && kTownClaimVanillaGateLen == 3
		   && !BuildTownClaimVanillaGate(tiny, sizeof(tiny)),
		      "gate: the vanilla thunk is xor eax,eax / ret");
	}
}

int main()
{
	CheckDecision();
	CheckNpcTown();
	CheckSkipFlag();
	CheckPick();
	CheckEligible();
	CheckSnapLea();
	CheckBytes();
	return CheckExit("town_claim_policy_units");
}
