#pragma once
#include "gui/settings_rows.h"
#include <vector>

// The PROD settings tab's sections, grouped for players rather than by
// config module. No game or Windows calls, so the host tests link it directly.

namespace keo_gui {

// One row's place on the PROD tab: its section's heading, then its key by
// the module's name and the key's own.
struct SettingsPlace
{
	const char* section;
	const char* module;
	const char* key;
};

// In display order, each section's rows together; a NULL section ends it.
extern const SettingsPlace kSettingsPlaces[];

} // namespace keo_gui

// The PROD rows: each kSettingsPlaces section with its shown keys (live, then
// startup-only), then any shown key no place names, under its module's title,
// so a player row missing from the table still reaches the tab.
void AddPlayerSections(SettingsStaging* staging, std::vector<SettingsRow>* out);
