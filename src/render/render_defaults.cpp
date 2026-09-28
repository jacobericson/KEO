#include "render/render_config.h"

// The compiled-in values and the live copy that starts from them. No Windows
// or game calls, so the host tests link it directly.

#ifdef ZONEOPT_DEBUG
#define RENDER_DEV_DEFAULT true
#else
#define RENDER_DEV_DEFAULT false
#endif

const RenderConfig kRenderDefaults =
{
	true,                // renderLevers
	true,                // reflectionHalfRate
	false,               // shadowReachDiag
	true,                // particleOffscreenSkip
	true,                // particleStepCap
	false,               // gpuParamLookupDiag
	RENDER_DEV_DEFAULT,  // renderDiag
	3.0f,                // particleStepCapSpeed
	0.5f,                // particleOffscreenSeconds
	10.0f,               // particleOffscreenMinAge
	"fire,smoke,torch,rain,weather",
	true,                // oldAnimSkip
	false,               // oldAnimDiag
	true,                // gpuParamCache
	true,                // shadowReachCull
	true,                // emptyPassSkip
	0.0f,                // foliagePageBudgetMs
	3.0f,                // foliageBudgetSpeed
	false,               // gpuUploadDiag
	true,                // gpuUploadSkip
	true,                // adoptOgrePurgeSkip
	120.0f               // adoptOgrePurgeMaxSkipSeconds
};

RenderConfig g_renderCfg = kRenderDefaults;

const RenderConfig& RenderConfigDefaults()
{
	return kRenderDefaults;
}
