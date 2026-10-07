// inst_empty_skip.cpp - instEmptySkip: an instance batch whose threaded cull
// left nothing visible skips its vertex buffer lock and upload (scene_levers.h).
#include "render/scene_levers.h"

#ifdef KEO_DEBUG

#include "render/scene_lever_policy.h"
#include "render/module_hooks.h"
#include "fixes/fixes_config.h"
#include "base/core.h"
#include <windows.h>
#include <sstream>
#include <string>
#include <string.h>

static const ModuleSite s_site = { "InstanceBatchHW::updateVertexBuffer", "OgreMain_x64.dll",
	"?updateVertexBuffer@InstanceBatchHW@Ogre@@IEAA_KPEAVCamera@2@PEBV32@@Z", 0x130490,
	{ 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7C,0x24,0x18,0x55 } };

typedef size_t (*UpdateVb_t)(void* batch, void* camera, const void* lodCamera);
static UpdateVb_t s_orig = NULL;
static const char* s_install = "not run";

// Published by the tick; the detour reads it first.
static volatile LONG s_mode = 0;

// Main thread only, except s_offMain.
static LONG s_calls = 0;
static LONG s_skipped = 0;
static LONG s_unthreaded = 0;
static volatile LONG s_offMain = 0;

static int s_seenMode = 0;
static double s_lastBeat = 0.0;
static const double kBeatSeconds = 60.0;

// The caller stores the return as the batch's instance count and queues the
// batch only when it is non-zero, so an empty batch's answer of 0 needs no
// upload: nothing draws the buffer.
static size_t hook_UpdateVertexBuffer(void* batch, void* camera, const void* lodCamera)
{
	if (!s_mode)
		return s_orig(batch, camera, lodCamera);
	const InstUpload d = InstUploadDecide(IsMainThread(), (const unsigned char*)batch);
	if (d == INST_OFF_MAIN)
	{
		InterlockedIncrement(&s_offMain);
		return s_orig(batch, camera, lodCamera);
	}
	++s_calls;
	if (d == INST_EMPTY)
	{
		++s_skipped;
		return 0;
	}
	if (d == INST_UNTHREADED)
		++s_unthreaded;
	return s_orig(batch, camera, lodCamera);
}

void InstallInstEmptySkip(int* installed, int*)
{
	(void)installed;
	const char* why = OgreSceneBuildRefusal(GetModuleHandleA(s_site.module));
	bool shared = false;
	if (!why && !InstallModuleHook(s_site, (void*)&hook_UpdateVertexBuffer, (void**)&s_orig, &shared))
		why = "hook";
	if (why)
	{
		s_install = why;
		LogMsg(std::string("InstSkip: install=refused(") + why + ")");
		return;
	}
	s_install = "ok";
	std::ostringstream line;
	line << "InstSkip: install=ok shared=" << (shared ? 1 : 0);
	LogMsg(line.str());
}

static bool InstallOk()
{
	return strcmp(s_install, "ok") == 0;
}

static void EmitHeartbeat()
{
	std::ostringstream line;
	line << "InstSkip: mode=" << (s_seenMode ? "on" : "off") << " install=";
	if (InstallOk())
		line << "ok";
	else
		line << "refused(" << s_install << ")";
	line << " calls=" << s_calls << " skipped=" << s_skipped << " unthreaded=" << s_unthreaded
	     << " offMain=" << InterlockedCompareExchange(&s_offMain, 0, 0);
	LogMsg(line.str());
}

void InstEmptySkipTick(double now)
{
	const int mode = (fixes::g_fixesCfg.cfg_instEmptySkip == 1 && InstallOk()) ? 1 : 0;
	bool beat = false;
	if (mode != s_seenMode)
	{
		InterlockedExchange(&s_mode, mode);
		s_seenMode = mode;
		beat = true;
	}
	if (beat || now - s_lastBeat >= kBeatSeconds)
	{
		s_lastBeat = now;
		EmitHeartbeat();
	}
}

#else  // !KEO_DEBUG

void InstallInstEmptySkip(int* installed, int*) { (void)installed; }
void InstEmptySkipTick(double now) { (void)now; }

#endif // KEO_DEBUG
