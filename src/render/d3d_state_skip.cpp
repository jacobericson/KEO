// d3d_state_skip.cpp - keeps an unchanged D3D11 blend, rasterizer or
// depth-stencil state object across a material change (d3dStateSkip, DEV).
#include "render/d3d_state_skip.h"

#ifdef KEO_DEBUG

#include "render/d3d_state_policy.h"
#include "render/module_hooks.h"
#include "fixes/fixes_config.h"
#include "base/core.h"
#include <windows.h>
#include <sstream>
#include <string>

// _render clears a set changed byte, makes the state object from its
// description and binds it when it differs from the bound mirror. Clearing the
// byte instead makes it AddRef and keep the mirror, which is right only while
// the mirror is the object an equal description made and is still bound: the
// shadows hold for one render system, device, context and epoch, and every path
// that can unbind or replace the context's state advances the epoch.
static const char* const D3D11_MODULE = "RenderSystem_Direct3D11_x64.dll";

static const ModuleSite s_renderSite =
{ "D3D11RenderSystem::_render", "RenderSystem_Direct3D11_x64.dll", NULL, 0x34170,
  { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 } };
// Calls ClearState on the context when the target is not NULL.
static const ModuleSite s_setRtSite =
{ "D3D11RenderSystem::_setRenderTarget", "RenderSystem_Direct3D11_x64.dll", NULL, 0x336E0,
  { 0x40,0x57,0x48,0x81,0xEC,0xD0,0x01,0x00,0x00,0x48,0xC7,0x44,0x24,0x30,0xFE,0xFF } };
// Flushes, clears and releases the context and device of a { device, context } pair.
static const ModuleSite s_releaseSite =
{ "D3D11Device release", "RenderSystem_Direct3D11_x64.dll", NULL, 0x1EA0,
  { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0x49 } };
// Clears and releases the context and makes the device again.
static const ModuleSite s_initSite =
{ "D3D11RenderSystem initialise", "RenderSystem_Direct3D11_x64.dll", NULL, 0x2E2F0,
  { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24 } };

static const uintptr_t D3D11_START_PLUGIN_RVA = 0x4D30;
static const uintptr_t D3D11_STOP_PLUGIN_RVA  = 0x4D80;

typedef void (*Render_t)(void* rs, const void* op);
typedef void (*SetRt_t)(void* rs, void* target);
typedef unsigned long long (*Release_t)(void* wrapper);
typedef void* (*Init_t)(void* rs, bool autoWindow, const void* title);

static Render_t  s_origRender  = NULL;
static SetRt_t   s_origSetRt   = NULL;
static Release_t s_origRelease = NULL;
static Init_t    s_origInit    = NULL;

static volatile LONG s_mode = 0;
static volatile LONG s_epoch = 0;
static D3dShadows s_shadows;   // main thread
static bool s_ready = false;   // all four hooked; written once by the install

// Main thread only.
static LONG s_calls = 0;
static LONG s_flagged[D3D_STATES] = { 0, 0, 0 };
static LONG s_skipped[D3D_STATES] = { 0, 0, 0 };
static LONG s_drops = 0;

static volatile LONG s_invalRt = 0;
static volatile LONG s_invalDev = 0;
static volatile LONG s_invalFrame = 0;
static volatile LONG s_offMain = 0;

// The install's refusal reason, NULL once it succeeded; set before any tick.
static const char* s_installWhy = "not run";
static int s_seenMode = 0;
static double s_lastBeat = 0.0;
static const double kBeatSeconds = 60.0;

// Main thread (Ogre's render calls). Off: one read and the forward.
static void hook_D3DRender(void* rs, const void* op)
{
	if (!s_mode || !s_ready)
	{
		s_origRender(rs, op);
		return;
	}
	if (!IsMainThread())
	{
		InterlockedIncrement(&s_offMain);
		InterlockedIncrement(&s_epoch);
		s_origRender(rs, op);
		return;
	}
	const D3dBefore b = D3dStateBefore((unsigned char*)rs, &s_shadows, s_epoch);
	++s_calls;
	if (b.dropped)
		++s_drops;
	for (int i = 0; i < D3D_STATES; ++i)
	{
		if (b.flagged & (1u << i))
			++s_flagged[i];
		if (b.skipped & (1u << i))
			++s_skipped[i];
	}
	// An exception from the original passes through: the recreated states'
	// shadows were already dropped, and its failure path leaves the mirror NULL.
	s_origRender(rs, op);
	D3dStateAfter((const unsigned char*)rs, &s_shadows, b.recreate);
}

// Its ClearState unbinds every state the mirrors still name.
static void hook_SetRenderTarget(void* rs, void* target)
{
	if (s_mode)
	{
		InterlockedIncrement(&s_epoch);
		InterlockedIncrement(&s_invalRt);
	}
	s_origSetRt(rs, target);
}

// The two device paths run at startup, configuration and teardown; any thread.
static unsigned long long hook_DeviceRelease(void* wrapper)
{
	InterlockedIncrement(&s_epoch);
	InterlockedIncrement(&s_invalDev);
	const unsigned long long r = s_origRelease(wrapper);
	InterlockedIncrement(&s_epoch);
	return r;
}

static void* hook_RsInitialise(void* rs, bool autoWindow, const void* title)
{
	InterlockedIncrement(&s_epoch);
	InterlockedIncrement(&s_invalDev);
	void* r = s_origInit(rs, autoWindow, title);
	InterlockedIncrement(&s_epoch);
	return r;
}

static LONG ReadCounter(volatile LONG* c)
{
	return InterlockedCompareExchange(c, 0, 0);
}

static void EmitHeartbeat()
{
	std::ostringstream os;
	os << "D3DState: mode=" << (s_seenMode ? "on" : "off") << " install=";
	if (s_installWhy)
		os << "refused(" << s_installWhy << ")";
	else
		os << "ok";
	os << " calls=" << s_calls
	   << " blend=" << s_skipped[D3D_BLEND] << "/" << s_flagged[D3D_BLEND]
	   << " raster=" << s_skipped[D3D_RASTER] << "/" << s_flagged[D3D_RASTER]
	   << " depth=" << s_skipped[D3D_DEPTH] << "/" << s_flagged[D3D_DEPTH]
	   << " drops=" << s_drops
	   << " invalRt=" << ReadCounter(&s_invalRt)
	   << " invalDev=" << ReadCounter(&s_invalDev)
	   << " invalFrame=" << ReadCounter(&s_invalFrame)
	   << " offMain=" << ReadCounter(&s_offMain);
	LogMsg(os.str());
}

// NULL when the loaded module is the build the offsets were read from, else the reason.
static const char* CheckBuild(HMODULE m)
{
	if (!m)
		return "no module";
	DWORD stamp = 0, size = 0;
	const uintptr_t base = (uintptr_t)m;
	if (!ReadModuleImageId(m, &stamp, &size)
	    || stamp != D3D_STATE_BUILD_STAMP || size != D3D_STATE_BUILD_SIZE
	    || (uintptr_t)GetProcAddress(m, "dllStartPlugin") != base + D3D11_START_PLUGIN_RVA
	    || (uintptr_t)GetProcAddress(m, "dllStopPlugin") != base + D3D11_STOP_PLUGIN_RVA)
		return "build mismatch";
	return NULL;
}

// Hooks the three invalidation paths first and _render only after all three;
// NULL on success, else the refused site's name.
static const char* HookSites(bool shared[4])
{
	if (!InstallModuleHook(s_setRtSite, (void*)&hook_SetRenderTarget, (void**)&s_origSetRt, &shared[1]))
		return s_setRtSite.name;
	if (!InstallModuleHook(s_releaseSite, (void*)&hook_DeviceRelease, (void**)&s_origRelease, &shared[2]))
		return s_releaseSite.name;
	if (!InstallModuleHook(s_initSite, (void*)&hook_RsInitialise, (void**)&s_origInit, &shared[3]))
		return s_initSite.name;
	if (!InstallModuleHook(s_renderSite, (void*)&hook_D3DRender, (void**)&s_origRender, &shared[0]))
		return s_renderSite.name;
	return NULL;
}

void InstallD3dStateSkip(int* installed, int*)
{
	(void)installed;
	D3dShadowsReset(&s_shadows);
	bool shared[4] = { false, false, false, false };
	const char* why = CheckBuild(GetModuleHandleA(D3D11_MODULE));
	if (!why)
		why = HookSites(shared);
	s_ready = why == NULL;
	s_installWhy = why;

	if (why)
	{
		LogMsg(std::string("D3DState: install=refused(") + why + ")");
		return;
	}
	std::ostringstream os;
	os << "D3DState: install=ok renderShared=" << (shared[0] ? 1 : 0) << " setRT=" << (shared[1] ? 1 : 0)
	   << " devRelease=" << (shared[2] ? 1 : 0) << " init=" << (shared[3] ? 1 : 0);
	LogMsg(os.str());
}

// A refused install publishes off whatever the key says. While on, each frame
// advances the epoch, so a state is made again at most once a frame.
void D3dStateSkipTick(double now)
{
	const int mode = (fixes::g_fixesCfg.cfg_d3dStateSkip == 1 && s_ready) ? 1 : 0;
	bool beat = false;
	if (mode != s_seenMode)
	{
		InterlockedExchange(&s_mode, mode);
		InterlockedIncrement(&s_epoch);
		s_seenMode = mode;
		beat = true;
	}
	if (mode)
	{
		InterlockedIncrement(&s_epoch);
		InterlockedIncrement(&s_invalFrame);
	}
	if (beat || now - s_lastBeat >= kBeatSeconds)
	{
		s_lastBeat = now;
		EmitHeartbeat();
	}
}

#else  // !KEO_DEBUG

void InstallD3dStateSkip(int* installed, int*) { (void)installed; }
void D3dStateSkipTick(double now) { (void)now; }

#endif // KEO_DEBUG
