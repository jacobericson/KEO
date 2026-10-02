#include "render/render_levers.h"
#include "render/render_config.h"
#include "game/game.h"
#include "base/core.h"

RenderStats g_renderStats;

#ifdef KEO_DEBUG
static const bool DEV_BUILD = true;
#else
static const bool DEV_BUILD = false;
#endif

struct RenderLever
{
	const char* key;
	bool*       flag;        // NULL for a lever keyed by a number
	bool      (*install)();
	const char* sites;       // what a refusal names
	bool        devOnly;     // a diagnostic installed in DEV builds only
	bool        installed;   // the sites were verified and hooked at startup
};

// One row per lever; a NULL key ends the table. Every lever is installed at
// startup whatever its key says, so its key can be switched at runtime; the
// detours check the key per call and pass straight through when it is off.
static RenderLever s_levers[] =
{
	{ "reflectionHalfRate",       &g_renderCfg.reflectionHalfRate,       &InstallReflectionLever,
	  "the water reflection hook", false, false },
	{ "particleOffscreenSkip",    &g_renderCfg.particleOffscreenSkip,    &InstallParticleLevers,
	  "the ParticleUniverse layout check", false, false },
	{ "particleStepCap",          &g_renderCfg.particleStepCap,          &InstallParticleStepCap,
	  "the ParticleSystem::_update hook", false, false },
	{ "shadowReachDiag",          &g_renderCfg.shadowReachDiag,          &InstallShadowReach,
	  "the shadow reach hook set", true, false },
	{ "gpuParamLookupDiag",       &g_renderCfg.gpuParamLookupDiag,       &InstallGpuParamsDiag,
	  "the D3D11 import-slot patch", true, false },
	{ "oldAnimSkip",              &g_renderCfg.oldAnimSkip,              &InstallOldAnimLever,
	  "the updateAllOldAnimations hook", false, false },
	{ "oldAnimDiag",              &g_renderCfg.oldAnimDiag,              &InstallOldAnimLever,
	  "the updateAllOldAnimations hook", true, false },
	{ "gpuParamCache",            &g_renderCfg.gpuParamCache,            &InstallGpuParamCache,
	  "the D3D11 import-slot patch or the GpuNamedConstants hooks", false, false },
	{ "shadowReachCull",          &g_renderCfg.shadowReachCull,          &InstallShadowReachCull,
	  "the shadow reach hook set", false, false },
	{ "emptyPassSkip",            &g_renderCfg.emptyPassSkip,            &InstallEmptyPassSkip,
	  "the OgreMain compositor exports", false, false },
	{ "foliagePageBudgetMs",      NULL,                                  &InstallFoliageBudget,
	  "the PagedGeometry::update hook", false, false },
	{ "gpuUploadDiag",            &g_renderCfg.gpuUploadDiag,            &InstallGpuUploadDiag,
	  "the D3D11 constant-buffer upload hook", true, false },
	{ "gpuUploadSkip",            &g_renderCfg.gpuUploadSkip,            &InstallGpuUploadSkip,
	  "the D3D11 constant-buffer upload and buffer destructor hooks or the GpuNamedConstants hooks", false, false },
	{ "adoptOgrePurgeSkip",       &g_renderCfg.adoptOgrePurgeSkip,       &InstallOgrePurgeSkip,
	  "the OgreMain resource-purge hooks", false, false },
	{ NULL, NULL, NULL, NULL, false, false }
};

void InstallRenderLevers(int* installed, int* wanted)
{
	*installed = 0;
	*wanted = 0;
	for (int i = 0; s_levers[i].key; ++i)
	{
		RenderLever& lever = s_levers[i];
		if (lever.devOnly && !DEV_BUILD)
		{
			if (lever.flag && *lever.flag)
				LogMsg(std::string("Render: lever ") + lever.key + " is DEV-only, not installed in this build");
			continue;
		}
		++*wanted;
		lever.installed = lever.install();
		if (lever.installed)
			++*installed;
		else
			LogMsg(std::string("Render: lever ") + lever.key + " refused, inert");
	}
}

std::string RenderLeverInertReason(const char* key)
{
	for (int i = 0; s_levers[i].key; ++i)
	{
		const RenderLever& lever = s_levers[i];
		if (strcmp(lever.key, key) != 0 || lever.installed)
			continue;
		if (!g_renderCfg.renderLevers)
			return "renderLevers was off at startup";
		if (lever.devOnly && !DEV_BUILD)
			return std::string(lever.sites) + " is installed in DEV builds only";
		return std::string(lever.sites) + " was refused at startup";
	}
	return std::string();
}

static const double RENDER_STATS_INTERVAL = 30.0;

// Main thread only: the current stats window. Counts calls to this tick, one
// per GameWorld::mainLoop_GPUSensitiveStuff iteration, not rendered frames.
static LONG   s_loops       = 0;
static double s_windowStart = -1.0;

void RenderLeversMainThreadTick(bool saveLoading)
{
	ParticleLevers_MainThreadTick(saveLoading);
	ShadowReach_MainThreadTick();
	GpuParams_MainThreadTick();
	OldAnim_MainThreadTick();
	EmptyPass_MainThreadTick();
	Foliage_MainThreadTick();
	GpuUpload_MainThreadTick();
	double now = ElapsedSec();
	if (s_windowStart < 0.0)
		s_windowStart = now;
	++s_loops;
	if (now - s_windowStart < RENDER_STATS_INTERVAL)
		return;
	std::string line = RenderLeverStatsLine();
	if (!line.empty())
		LogMsg(line);
}

// Every window ends here whether or not the line is printed: each counter is
// read and zeroed, so the next line covers one window and no count can wrap.
std::string RenderLeverStatsLine()
{
	double now = ElapsedSec();
	double window = (s_windowStart < 0.0) ? 0.0 : now - s_windowStart;
	LONG loops = s_loops;
	s_loops = 0;
	s_windowStart = now;

	LONG reflKept      = InterlockedExchange(&g_renderStats.reflKept, 0);
	LONG reflSkipped   = InterlockedExchange(&g_renderStats.reflSkipped, 0);
	LONG reflFog       = InterlockedExchange(&g_renderStats.reflFog, 0);
	LONG fxSet         = InterlockedExchange(&g_renderStats.fxTimeoutSet, 0);
	LONG fxCleared     = InterlockedExchange(&g_renderStats.fxTimeoutCleared, 0);
	LONG fxFull        = InterlockedExchange(&g_renderStats.fxRecordFull, 0);
	LONG fxTimedOut    = g_renderStats.fxTimedOut;   // a state, not a count
	LONG fxCapped      = InterlockedExchange(&g_renderStats.fxStepCapped, 0);
	LONG lookupCalls   = InterlockedExchange(&g_renderStats.lookupCalls, 0);
	LONGLONG lookupTks = InterlockedExchange64(&g_renderStats.lookupTicks, 0);
	LONG lookupHits    = InterlockedExchange(&g_renderStats.lookupHits, 0);
	LONG lookupMisses  = InterlockedExchange(&g_renderStats.lookupMisses, 0);
	LONG lookupOff     = InterlockedExchange(&g_renderStats.lookupOffThread, 0);
	std::string reach  = ShadowReachStatsToken(window);
	std::string oldAnim = OldAnimStatsToken(loops);
	std::string empty  = EmptyPassStatsToken();
	std::string foliage = FoliageStatsToken();
	std::string upload = GpuUploadStatsToken(loops);
	std::string ogrePurge = OgrePurgeStatsToken();

	if (!g_renderCfg.renderLevers || !g_renderCfg.renderDiag)
		return std::string();

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	double lookupMs = freq.QuadPart ? (double)lookupTks * 1000.0 / (double)freq.QuadPart : 0.0;
	double perLoop = loops > 0 ? 1.0 / (double)loops : 0.0;

	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(1)
	   << "Render: window=" << window << "s loops=" << loops
	   << " refl=kept" << reflKept << "/skip" << reflSkipped << " reflFog=" << reflFog
	   << " fxTimeout=set" << fxSet << "/clr" << fxCleared << "/full" << fxFull << "/out" << fxTimedOut
	   << " fxStepCapped=" << fxCapped;
	// The diagnostic counts every lookup; without it the cache's own counts
	// add up to the same total.
	bool diagOn = DEV_BUILD && g_renderCfg.gpuParamLookupDiag;
	if (diagOn || GpuParamCacheActive())
	{
		LONG calls = diagOn ? lookupCalls : lookupHits + lookupMisses + lookupOff;
		ss << " lookup=" << (double)calls * perLoop << "/" << (double)lookupHits * perLoop << "/loop";
		if (diagOn)
			ss << " " << std::setprecision(3) << lookupMs * perLoop << "ms/loop";
		if (lookupOff)
			ss << " lookupOffThread=" << lookupOff;
	}
	ss << reach
	   << oldAnim
	   << empty
	   << foliage
	   << upload
	   << ogrePurge;
	return ss.str();
}
