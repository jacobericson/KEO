// d3d_state_policy.cpp - the D3D11 state-object keep rule, its shadows and the
// render system build check.
#include "render/d3d_state_policy.h"
#include <string.h>

D3dStateLayout D3dLayout(int state)
{
	D3dStateLayout l = { 0, 0, 0, 0 };
	if (state == D3D_BLEND)
	{
		l.changed = RS_BLEND_CHANGED; l.desc = RS_BLEND_DESC; l.size = 264; l.bound = RS_BLEND_BOUND;
	}
	else if (state == D3D_RASTER)
	{
		l.changed = RS_RASTER_CHANGED; l.desc = RS_RASTER_DESC; l.size = 40; l.bound = RS_RASTER_BOUND;
	}
	else if (state == D3D_DEPTH)
	{
		l.changed = RS_DEPTH_CHANGED; l.desc = RS_DEPTH_DESC; l.size = 52; l.bound = RS_DEPTH_BOUND;
	}
	return l;
}

void D3dShadowsReset(D3dShadows* sh)
{
	memset(sh, 0, sizeof(*sh));
	for (int i = 0; i < D3D_STATES; ++i)
		sh->s[i].held = false;
	sh->rs = NULL;   // the key matches no render system, so the first call drops
	sh->device = NULL;
	sh->context = NULL;
	sh->epoch = 0;
}

bool D3dStateSkips(bool held, bool boundSet, bool descEqual, bool refEqual)
{
	return held && boundSet && descEqual && refEqual;
}

D3dBefore D3dStateBefore(unsigned char* rs, D3dShadows* sh, long epoch)
{
	D3dBefore b = { 0, 0, 0, false };
	const void* device = *(void* const*)(rs + RS_DEVICE);
	const void* context = *(void* const*)(rs + RS_CONTEXT);
	if (sh->rs != rs || sh->device != device || sh->context != context || sh->epoch != epoch)
	{
		for (int i = 0; i < D3D_STATES; ++i)
			sh->s[i].held = false;
		sh->rs = rs;
		sh->device = device;
		sh->context = context;
		sh->epoch = epoch;
		b.dropped = true;
	}
	for (int i = 0; i < D3D_STATES; ++i)
	{
		const D3dStateLayout l = D3dLayout(i);
		if (rs[l.changed] == 0)
			continue;
		const unsigned bit = 1u << i;
		b.flagged |= bit;
		D3dShadow& s = sh->s[i];
		const bool bound = *(const unsigned long long*)(rs + l.bound) != 0;
		const bool desc = memcmp(rs + l.desc, s.desc, l.size) == 0;
		const bool ref = i != D3D_DEPTH || *(const unsigned*)(rs + RS_STENCIL_REF) == s.ref;
		if (D3dStateSkips(s.held, bound, desc, ref))
		{
			rs[l.changed] = 0;
			b.skipped |= bit;
		}
		else
		{
			s.held = false;
			b.recreate |= bit;
		}
	}
	return b;
}

void D3dStateAfter(const unsigned char* rs, D3dShadows* sh, unsigned recreate)
{
	for (int i = 0; i < D3D_STATES; ++i)
	{
		if (!(recreate & (1u << i)))
			continue;
		const D3dStateLayout l = D3dLayout(i);
		D3dShadow& s = sh->s[i];
		memcpy(s.desc, rs + l.desc, l.size);
		if (i == D3D_DEPTH)
			s.ref = *(const unsigned*)(rs + RS_STENCIL_REF);
		s.held = true;
	}
}
