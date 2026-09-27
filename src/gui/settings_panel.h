#pragma once

// The ZoneOpt tab in the game's Options window. Its rows edit a staging copy
// of the settings; closing Options applies the render keys that changed and
// writes KenshiZoneOpt.ini. Main thread only.

// Startup, after LoadConfig and the build gate: hooks OptionsWindow::create
// and ::saveOptions unless settingsPanel is off. Either hook refused leaves
// the panel off and the rest of the plugin running.
void InstallSettingsPanel();

// The banner token: "ok", "off(ini)" or "off(hook)".
const char* SettingsPanelToken();
