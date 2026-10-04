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

// The heading of the shown keys no place names.
extern const char* const OTHER_SETTINGS_TITLE;

// The PROD rows: each section of places with its shown keys, ordered by
// AddSectionRows, then every shown key places does not name under
// OTHER_SETTINGS_TITLE, so a row missing from the table still reaches the tab.
void AddPlayerSections(const SettingsPlace* places, SettingsStaging* staging, std::vector<SettingsRow>* out);

} // namespace keo_gui
