// The D3D11 state-object keep rule: the rule itself, the before step's key and
// per-state decisions, the after step's shadows, the render system layout the
// offsets describe, and the PE build check.

#include "render/d3d_state_policy.h"
#include <string.h>

#include "check.h"

static const char* const SUITE_NAME = "d3d_state_policy_units";

// A fake render system: the device, context and bound objects are 8-byte
// values, the descriptions byte patterns.
struct FakeRs
{
	unsigned long long q[RS_STATE_END / 8];
	unsigned char* p() { return (unsigned char*)q; }
};

static void PutQ(FakeRs* rs, size_t off, unsigned long long v)
{
	memcpy(rs->p() + off, &v, sizeof(v));
}

static unsigned long long GetQ(FakeRs* rs, size_t off)
{
	unsigned long long v;
	memcpy(&v, rs->p() + off, sizeof(v));
	return v;
}

static void PutRef(FakeRs* rs, unsigned ref)
{
	memcpy(rs->p() + RS_STENCIL_REF, &ref, sizeof(ref));
}

static void Flag(FakeRs* rs, int state)
{
	rs->p()[D3dLayout(state).changed] = 1;
}

static unsigned char Flagged(FakeRs* rs, int state)
{
	return rs->p()[D3dLayout(state).changed];
}

static void Fresh(FakeRs* rs, D3dShadows* sh)
{
	memset(rs, 0, sizeof(*rs));
	PutQ(rs, RS_DEVICE, 0x1000);
	PutQ(rs, RS_CONTEXT, 0x2000);
	for (int i = 0; i < D3D_STATES; ++i)
	{
		const D3dStateLayout l = D3dLayout(i);
		for (size_t b = 0; b < l.size; ++b)
			rs->p()[l.desc + b] = (unsigned char)(0x11 * (i + 1) + b);
	}
	PutRef(rs, 3);
	D3dShadowsReset(sh);
}

// What the original does with a flagged state: makes a new object and binds it.
static unsigned long long s_nextObject = 0x9000;
static void SimOriginal(FakeRs* rs)
{
	for (int i = 0; i < D3D_STATES; ++i)
	{
		const D3dStateLayout l = D3dLayout(i);
		if (!rs->p()[l.changed])
			continue;
		rs->p()[l.changed] = 0;
		PutQ(rs, l.bound, s_nextObject);
		s_nextObject += 0x10;
	}
}

// One pass with every state flagged: all three made, bound and shadowed.
static void Prime(FakeRs* rs, D3dShadows* sh, long epoch)
{
	for (int i = 0; i < D3D_STATES; ++i)
		Flag(rs, i);
	D3dBefore b = D3dStateBefore(rs->p(), sh, epoch);
	SimOriginal(rs);
	D3dStateAfter(rs->p(), sh, b.recreate);
}

static bool AllHeld(const D3dShadows& sh)
{
	return sh.s[D3D_BLEND].held && sh.s[D3D_RASTER].held && sh.s[D3D_DEPTH].held;
}

static void CheckRule()
{
	Check(D3dStateSkips(true, true, true, true), "rule: a held, bound, unchanged state is kept");
	Check(!D3dStateSkips(true, false, true, true), "rule: nothing bound is never kept");
	Check(!D3dStateSkips(false, true, true, true), "rule: no shadow is never kept");
	Check(!D3dStateSkips(true, true, false, true), "rule: a changed description is not kept");
	Check(!D3dStateSkips(true, true, true, false), "rule: a changed stencil reference is not kept");
}

// Drops every shadow when the key part changed by `change` differs.
static bool DropsAfter(int change)
{
	FakeRs rs;
	D3dShadows sh;
	Fresh(&rs, &sh);
	Prime(&rs, &sh, 5);
	long epoch = 5;
	FakeRs other;
	unsigned char* target = rs.p();
	if (change == 0) epoch = 6;
	if (change == 1) PutQ(&rs, RS_CONTEXT, 0x2100);
	if (change == 2) PutQ(&rs, RS_DEVICE, 0x1100);
	if (change == 3)
	{
		memcpy(&other, &rs, sizeof(rs));
		target = other.p();
	}
	for (int i = 0; i < D3D_STATES; ++i)
		target[D3dLayout(i).changed] = 1;
	D3dBefore b = D3dStateBefore(target, &sh, epoch);
	return b.dropped && b.skipped == 0 && b.recreate == 7 && b.flagged == 7;
}

static void CheckBefore()
{
	FakeRs rs;
	D3dShadows sh;

	Fresh(&rs, &sh);
	for (int i = 0; i < D3D_STATES; ++i)
		Flag(&rs, i);
	D3dBefore b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.dropped && b.flagged == 7 && b.skipped == 0 && b.recreate == 7
	      && !sh.s[D3D_BLEND].held && !sh.s[D3D_RASTER].held && !sh.s[D3D_DEPTH].held
	      && Flagged(&rs, D3D_BLEND) && Flagged(&rs, D3D_RASTER) && Flagged(&rs, D3D_DEPTH),
	      "before: a first call holds nothing and keeps nothing");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_BLEND);
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.flagged == 1 && !(b.skipped & 6) && !(b.recreate & 6) && !Flagged(&rs, D3D_RASTER)
	      && !Flagged(&rs, D3D_DEPTH) && sh.s[D3D_RASTER].held && sh.s[D3D_DEPTH].held,
	      "before: an unflagged state is left alone");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_BLEND);
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(!b.dropped && b.skipped == 1 && b.recreate == 0 && !Flagged(&rs, D3D_BLEND),
	      "before: a flagged unchanged blend state has its flag cleared");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_RASTER);
	rs.p()[RS_RASTER_DESC + 4] ^= 0x01;
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.skipped == 0 && b.recreate == 2 && Flagged(&rs, D3D_RASTER),
	      "before: a flagged changed rasterizer state keeps its flag and is recreated");
	Check(!sh.s[D3D_RASTER].held, "before: a recreated state's shadow is not held until after");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_DEPTH);
	PutRef(&rs, 7);
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.skipped == 0 && b.recreate == 4 && Flagged(&rs, D3D_DEPTH),
	      "before: the stencil reference counts for the depth-stencil state");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_BLEND);
	Flag(&rs, D3D_RASTER);
	PutRef(&rs, 7);
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.skipped == 3 && b.recreate == 0,
	      "before: the stencil reference does not count for the blend and rasterizer states");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	Flag(&rs, D3D_BLEND);
	PutQ(&rs, RS_BLEND_BOUND, 0);
	b = D3dStateBefore(rs.p(), &sh, 1);
	Check(b.skipped == 0 && b.recreate == 1 && Flagged(&rs, D3D_BLEND),
	      "before: a nulled bound object is recreated");

	Check(DropsAfter(0), "before: a new epoch drops every shadow");
	Check(DropsAfter(1), "before: a new context drops every shadow");
	Check(DropsAfter(2), "before: a new device drops every shadow");
	Check(DropsAfter(3), "before: another render system drops every shadow");
}

static void CheckAfter()
{
	FakeRs rs;
	D3dShadows sh;

	Fresh(&rs, &sh);
	PutRef(&rs, 0x55);
	Prime(&rs, &sh, 1);
	bool same = AllHeld(sh);
	for (int i = 0; i < D3D_STATES; ++i)
	{
		const D3dStateLayout l = D3dLayout(i);
		same = same && memcmp(sh.s[i].desc, rs.p() + l.desc, l.size) == 0;
	}
	Check(same, "after: a recreated state's description becomes its shadow");
	Check(sh.s[D3D_DEPTH].ref == 0x55, "after: the depth-stencil shadow takes the stencil reference");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	const D3dShadow kept = sh.s[D3D_BLEND];
	Flag(&rs, D3D_BLEND);
	D3dBefore b = D3dStateBefore(rs.p(), &sh, 1);
	SimOriginal(&rs);
	// The live description moves on after the skip; only a recreated state may take it.
	rs.p()[RS_BLEND_DESC] ^= 0xFF;
	D3dStateAfter(rs.p(), &sh, b.recreate);
	Check(b.skipped == 1 && sh.s[D3D_BLEND].held && sh.s[D3D_BLEND].ref == kept.ref
	      && memcmp(kept.desc, sh.s[D3D_BLEND].desc, sizeof(kept.desc)) == 0,
	      "after: a kept state's shadow is unchanged");

	Fresh(&rs, &sh);
	Prime(&rs, &sh, 1);
	const unsigned long long blend = GetQ(&rs, RS_BLEND_BOUND);
	const unsigned long long raster = GetQ(&rs, RS_RASTER_BOUND);
	const unsigned long long depth = GetQ(&rs, RS_DEPTH_BOUND);
	for (int i = 0; i < D3D_STATES; ++i)
		Flag(&rs, i);
	b = D3dStateBefore(rs.p(), &sh, 1);
	SimOriginal(&rs);
	D3dStateAfter(rs.p(), &sh, b.recreate);
	Check(b.flagged == 7 && b.skipped == 7 && b.recreate == 0 && GetQ(&rs, RS_BLEND_BOUND) == blend
	      && GetQ(&rs, RS_RASTER_BOUND) == raster && GetQ(&rs, RS_DEPTH_BOUND) == depth,
	      "after: two passes with one material keep all three states the second time");
}

static void CheckLayout()
{
	bool ends = true, inside = true;
	for (int i = 0; i < D3D_STATES; ++i)
	{
		const D3dStateLayout l = D3dLayout(i);
		ends = ends && l.size > 0 && l.size <= D3D_DESC_MAX && l.desc + l.size == l.changed;
		inside = inside && l.bound + 8 <= RS_STATE_END && l.bound > l.changed;
	}
	const D3dStateLayout none = D3dLayout(D3D_STATES);
	Check(ends, "layout: each description ends at its changed byte");
	Check(RS_STENCIL_REF + 4 == RS_DEPTH_DESC,
	      "layout: the stencil reference sits just before the depth-stencil description");
	Check(inside && none.changed == 0 && none.desc == 0 && none.size == 0 && none.bound == 0
	      && RS_CONTEXT + 8 <= RS_BLEND_DESC,
	      "layout: the mirrors lie inside the fake render system");
}

int main()
{
	CheckRule();
	CheckBefore();
	CheckAfter();
	CheckLayout();
	return CheckExit(SUITE_NAME);
}
