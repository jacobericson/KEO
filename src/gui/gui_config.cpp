// gui_config.cpp - defaults and INI rows for the gui module.
#include "gui/gui_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

namespace zoneopt_gui {

const GuiConfig kGuiDefaults =
{
	true, // settingsPanelEnabled
};

GuiConfig g_guiCfg = kGuiDefaults;
} // namespace zoneopt_gui

namespace gui_config_detail {
union GuiConfigPodCheck { zoneopt_gui::GuiConfig s; };
} // namespace gui_config_detail
using namespace gui_config_detail;

namespace zoneopt_gui {

const ConfigKey g_guiConfigKeys[] =
{
	CFG_OBOOL("settingsPanel", GuiConfig, settingsPanelEnabled,         DOC, SHOW,
	  "ZoneOpt settings tab",
	  "This tab in the game's Options window."),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace zoneopt_gui
