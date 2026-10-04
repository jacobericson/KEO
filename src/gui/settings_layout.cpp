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

} // namespace keo_gui

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

void AddPlayerSections(SettingsStaging* staging, std::vector<SettingsRow>* out)
{
	using keo_gui::kSettingsPlaces;
	bool placed[CONFIG_MODULE_MAX][CONFIG_STAGE_MAX];
	memset(placed, 0, sizeof(placed));

	for (int p = 0; kSettingsPlaces[p].section;)
	{
		const char* title = kSettingsPlaces[p].section;
		std::vector<SettingsKeyRef> keys;
		for (; kSettingsPlaces[p].section && strcmp(kSettingsPlaces[p].section, title) == 0; ++p)
		{
			int m = ModuleNamed(kSettingsPlaces[p].module);
			int k = m >= 0 ? KeyNamed(kConfigModules[m], kSettingsPlaces[p].key) : -1;
			if (k < 0 || placed[m][k])
				continue;
			placed[m][k] = true;
			SettingsKeyRef ref = { &kConfigModules[m], k, &staging->module[m] };
			keys.push_back(ref);
		}
		AddSectionRows(title, keys, false, out);
	}

	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		std::vector<SettingsKeyRef> keys;
		for (int i = 0; i < CONFIG_STAGE_MAX && kConfigModules[m].keys[i].name; ++i)
		{
			if (placed[m][i])
				continue;
			SettingsKeyRef ref = { &kConfigModules[m], i, &staging->module[m] };
			keys.push_back(ref);
		}
		AddSectionRows(kConfigModules[m].title, keys, false, out);
	}
}
