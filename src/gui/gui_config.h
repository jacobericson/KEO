// gui_config.h - the gui module's INI storage, defaults and table.
#pragma once
#include "base/config_table.h"

namespace keo_gui {

// Starts as a copy of kGuiDefaults, then written by LoadConfig on the main
// thread before any hook installs; read-only afterwards, on any thread.
struct GuiConfig
{
	// settingsPanel: the ZoneOpt tab in the game's Options window (src/gui/).
	// On by default; read at startup only.
	bool settingsPanelEnabled;
};

extern GuiConfig g_guiCfg;
extern const GuiConfig kGuiDefaults;
extern const ConfigKey g_guiConfigKeys[];

} // namespace keo_gui
