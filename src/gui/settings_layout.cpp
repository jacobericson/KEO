#include "gui/settings_layout.h"
#include <cstring>

namespace keo_gui {

const SettingsPlace kSettingsPlaces[] =
{
	{ "Zone loading",   "zone",     "preload" },
	{ "Zone loading",   "zone",     "zoneLifeRetainRadius" },
	{ "Zone loading",   "navmesh",  "navmeshWorkerCount" },
	{ "Zone loading",   "navmesh",  "navmeshDiskCacheMaxMB" },
	{ "Performance",    "render",   "particleStepCap" },
	{ "Performance",    "render",   "foliagePageBudgetMs" },
	{ "Performance",    "render",   "renderLevers" },
	{ "Squad movement", "movement", "groupCohesion" },
	{ "Squad movement", "movement", "k7PostDeathHold" },
	{ NULL, NULL, NULL }
};

const char* const OTHER_SETTINGS_TITLE = "Other settings";

static int ModuleNamed(const char* name)
{
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		if (strcmp(kConfigModules[m].name, name) == 0)
			return m;
	}
	return -1;
}

static int KeyNamed(const ConfigModule& m, const char* name)
{
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		if (!m.keys[i].retired && strcmp(m.keys[i].name, name) == 0)
			return i;
	}
	return -1;
}

void AddPlayerSections(const SettingsPlace* places, SettingsStaging* staging, std::vector<SettingsRow>* out)
{
	bool placed[CONFIG_MODULE_MAX][CONFIG_STAGE_MAX];
	memset(placed, 0, sizeof(placed));

	for (int p = 0; places[p].section;)
	{
		const char* title = places[p].section;
		std::vector<SettingsKeyRef> keys;
		for (; places[p].section && strcmp(places[p].section, title) == 0; ++p)
		{
			int m = ModuleNamed(places[p].module);
			int k = m >= 0 ? KeyNamed(kConfigModules[m], places[p].key) : -1;
			if (k < 0 || placed[m][k])
				continue;
			placed[m][k] = true;
			SettingsKeyRef ref = { &kConfigModules[m], k, &staging->module[m] };
			keys.push_back(ref);
		}
		AddSectionRows(title, keys, false, out);
	}

	std::vector<SettingsKeyRef> others;
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		for (int i = 0; i < CONFIG_STAGE_MAX && kConfigModules[m].keys[i].name; ++i)
		{
			if (placed[m][i])
				continue;
			SettingsKeyRef ref = { &kConfigModules[m], i, &staging->module[m] };
			others.push_back(ref);
		}
	}
	AddSectionRows(OTHER_SETTINGS_TITLE, others, false, out);
}

} // namespace keo_gui
