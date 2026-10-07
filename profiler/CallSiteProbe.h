// CallSiteProbe - time a call at its call site.
//
// Direct sites: a probe rewrites the call's rel32 (`E8 rel32`) so it lands on
// a 16-byte stub in a page allocated within +-2 GB of the module
// (`FF 25 00000000` + absolute address), which jumps to a per-probe C++
// wrapper. The wrapper reports entry, calls the original target (the address
// the call decoded to, usually a `j_` thunk, so any detour behind it stays
// inside the timing), then reports the two QPC stamps around that call.
//
// Virtual sites: `mov rax,[rcx]` followed by `call qword ptr [rax+disp32]`
// (48 8B 01 FF 90 disp32). The 6-byte call becomes `E8 rel32` + `90`, and the
// wrapper makes the same virtual call itself through [*rcx + disp32], so every
// class that reaches the site is timed.
//
// Indirect sites: `call qword ptr [rip+disp32]` (FF 15 disp32), e.g. a call
// through an import slot. The 6-byte call becomes `E8 rel32` + `90`, and the
// wrapper calls through the same pointer slot, read at call time, so anything
// that later rewrites the slot stays behind the probe.
//
// No function prologue is touched. Generic: this file knows nothing about the
// game. The caller passes a table of sites and receives the probe id assigned
// to each installed site.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace CallSiteProbe
{
	typedef unsigned __int64 U64;

	const int MAX_PROBES = 96;

	// How the callee takes its arguments and returns. Wrappers forward
	// rcx/rdx/r8/r9 (SHAPE_INT), rcx/xmm1/r8/r9 (SHAPE_FLOAT: the second
	// argument is a float), rcx/rdx/r8/xmm3 (SHAPE_FLOAT4: the fourth argument
	// is a float), or rcx/rdx/r8/r9 with the result returned in xmm0
	// (SHAPE_RETFLOAT). Forwarding more registers than a callee uses is
	// harmless; no probed callee may take stack arguments, and only a
	// SHAPE_RETFLOAT callee may return in xmm0.
	enum Shape
	{
		SHAPE_INT      = 0,
		SHAPE_FLOAT    = 1,
		SHAPE_FLOAT4   = 2,
		SHAPE_RETFLOAT = 3
	};

	struct Site
	{
		const char* name;
		size_t      siteRva;    // RVA of the E8 (direct), FF 90 (virtual) or FF 15 (indirect) instruction
		size_t      targetRva;  // direct: RVA the call must decode to; indirect: RVA of the
		                        // pointer slot its disp32 must decode to; virtual: 0
		int         shape;      // Shape (virtual and indirect sites: SHAPE_INT)
		int         tag;        // caller's own identifier, passed back unchanged
		size_t      vslot;      // virtual sites: the call's disp32 (vtable byte offset); 0 = not virtual
		int         indirect;   // 1 = indirect site (FF 15 disp32); 0 = direct or virtual
		int         looseVirtual;// virtual: caller verified rcx remains the object across argument setup

		// Filled by Install:
		int         id;         // probe id given to the callbacks, -1 if not installed
		void*       orig;       // direct: absolute original target; indirect: the pointer
		                        // slot; virtual: NULL
		char        status[96]; // "ok" or the reason it was skipped
	};

	// Called on whatever thread executes the call site. `a`..`d` are the four
	// integer argument registers rcx/rdx/r8/r9 (b is 0 for SHAPE_FLOAT, whose
	// second argument is in xmm1). t0/t1 are the QPC stamps taken immediately
	// before and after the original call.
	typedef void (*EnterFn)(int id, U64 a, U64 b, U64 c, U64 d);
	typedef void (*ExitFn)(int id, U64 ret, LONGLONG t0, LONGLONG t1);

	// Must be set before Install.
	void SetCallbacks(EnterFn onEnter, ExitFn onExit);

	// Verifies and installs every site that passes its checks: a direct site's
	// byte 0 is E8 and the call decodes to targetRva; a virtual site's bytes
	// are 48 8B 01 FF 90 with disp32 == vslot; an indirect site's bytes are
	// FF 15 and its disp32 decodes to targetRva. A virtual or indirect site must
	// be SHAPE_INT. Run it once, while none of the sites can execute. Returns
	// the number installed; each site's status says why not.
	int Install(HMODULE module, Site* sites, int count);

	// The stub page (NULL until a site was installed).
	const void* StubPage();

	// Tag of the site installed under probe id `id`, or -1.
	int TagOf(int id);
}
