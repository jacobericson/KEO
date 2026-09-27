#include "render/render_config.h"
#include "render/render_keys.h"
#include "base/config.h"

void ClampRenderConfig()
{
	std::vector<std::string> notes;
	ClampRenderValues(&g_renderCfg, RenderConfigDefaults(), &notes);
	for (size_t i = 0; i < notes.size(); ++i)
		LogMsg("Config: " + notes[i]);
}

void LogRenderConfig()
{
	LogMsg("Config: " + FormatRenderConfig(g_renderCfg));
}
