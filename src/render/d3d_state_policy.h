#ifndef KEO_RENDER_D3D_STATE_POLICY_H
#define KEO_RENDER_D3D_STATE_POLICY_H

// When D3D11RenderSystem::_render may keep a blend, rasterizer or
// depth-stencil state object instead of making and binding it again: the
// state's description (and, for the depth-stencil state, the stencil
// reference) equals the one the still-bound object was made from, and nothing
// has cleared or replaced the context since. Pure: no game, Windows or D3D
// header.

#include <stddef.h>

// RenderSystem_Direct3D11_x64.dll: its PE TimeDateStamp and SizeOfImage.
const unsigned long D3D_STATE_BUILD_STAMP = 0x5CA5F9E9UL;
const unsigned long D3D_STATE_BUILD_SIZE  = 0xBF000UL;

// D3D11RenderSystem, as _render reads it: the device and context, each state's
// description, its changed byte and the object last bound for it.
const size_t RS_DEVICE         = 0x420;
const size_t RS_CONTEXT        = 0x428;
const size_t RS_BLEND_DESC     = 0x4A8;
const size_t RS_BLEND_CHANGED  = 0x5B0;
const size_t RS_RASTER_DESC    = 0x5B4;
const size_t RS_RASTER_CHANGED = 0x5DC;
const size_t RS_STENCIL_REF    = 0x5E0;
const size_t RS_DEPTH_DESC     = 0x5E4;
const size_t RS_DEPTH_CHANGED  = 0x618;
const size_t RS_BLEND_BOUND    = 0x738;
const size_t RS_RASTER_BOUND   = 0x740;
const size_t RS_DEPTH_BOUND    = 0x748;
const size_t RS_STATE_END      = 0x750;

enum D3dState { D3D_BLEND, D3D_RASTER, D3D_DEPTH, D3D_STATES };
const size_t D3D_DESC_MAX = 264;

struct D3dStateLayout
{
	size_t changed, desc, size, bound;
};
// The layout of one state (D3D_BLEND .. D3D_DEPTH).
D3dStateLayout D3dLayout(int state);

// The description each bound object was made from, valid only for the
// render system, device, context and epoch it was taken under.
struct D3dShadow
{
	unsigned char desc[D3D_DESC_MAX];
	unsigned      ref;    // depth-stencil only: the stencil reference bound with it
	bool          held;
};
struct D3dShadows
{
	D3dShadow   s[D3D_STATES];
	const void* rs;
	const void* device;
	const void* context;
	long        epoch;
};
void D3dShadowsReset(D3dShadows* sh);

// A flagged state is kept when its shadow is held, an object is bound and the
// description and reference are unchanged.
bool D3dStateSkips(bool held, bool boundSet, bool descEqual, bool refEqual);

// Bit (1 << state) masks.
struct D3dBefore
{
	unsigned flagged;    // changed byte set on entry
	unsigned skipped;    // flag cleared: the original keeps the bound object
	unsigned recreate;   // left to the original; shadow not held until D3dStateAfter
	bool     dropped;    // the key differed and every shadow was dropped first
};
// Before the original: drops every shadow when the render system, device,
// context or epoch differs from the shadows' key; then, per flagged state,
// clears its changed byte (a skip) or marks its shadow not held.
D3dBefore D3dStateBefore(unsigned char* rs, D3dShadows* sh, long epoch);
// After the original returned: each recreated state's description (and the
// stencil reference) becomes its held shadow.
void D3dStateAfter(const unsigned char* rs, D3dShadows* sh, unsigned recreate);

// A PE image's TimeDateStamp and SizeOfImage from its first len bytes; false
// for anything but a PE32+ header inside len.
bool D3dPeStampAndSize(const unsigned char* image, size_t len, unsigned long* stamp, unsigned long* size);

#endif // KEO_RENDER_D3D_STATE_POLICY_H
