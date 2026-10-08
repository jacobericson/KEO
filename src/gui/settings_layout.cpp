#include "gui/settings_layout.h"
#include <cstring>

namespace keo_gui {

const SettingsPlace kSettingsPlaces[] =
{
	{ "Zone loading",   "zone",     "preload" },
	{ "Zone loading",   "navmesh",  "navmeshWorkerCount" },
	{ "Zone loading",   "zone",     "zoneLifeRetainRadius" },
	{ "Zone loading",   "zone",     "zoneLifeSquadRadius" },
	{ "Zone loading",   "zone",     "zoneRetentionMaxHeld" },
	{ "Zone loading",   "planner",  "plannerAheadTiles" },
	{ "Zone loading",   "navmesh",  "navmeshDiskCacheMaxMB" },
	{ "Performance",    "render",   "particleStepCap" },
	{ "Performance",    "render",   "foliagePageBudgetMs" },
	{ "Squad movement", "movement", "groupCohesion" },
	{ "Squad movement", "movement", "formationGatherPace" },
	{ "Squad movement", "movement", "k7PostDeathHold" },
	{ "Squad movement", "planner", "plannerMode" },
	{ "Gameplay fixes",     "fixes",     "townClaimFix" },
	{ "Gameplay fixes",     "fixes",     "throwOutFix",         "throwOut" },
	{ "Gameplay fixes",     "fixes",     "throwOutHoldMinutes", "throwOut" },
	{ "Gameplay fixes",     "navmesh",   "wallSpliceFix" },
	{ "Backpacks and jobs", "inventory", "backpackFixes",       "backpack" },
	{ "Backpacks and jobs", "inventory", "operatorHoldUntil",   "backpack" },
	{ NULL, NULL, NULL, NULL }
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

// The index of the first place carrying place p's group; p must carry one.
static int GroupFirst(const SettingsPlace* places, int p)
{
	int first = 0;
	while (!places[first].group || strcmp(places[first].group, places[p].group) != 0)
		++first;
	return first;
}

bool SettingsGroupsValid(const SettingsPlace* places)
{
	for (int p = 0; places[p].section; ++p)
	{
		if (!places[p].group)
			continue;
		const int first = GroupFirst(places, p);
		if (strcmp(places[first].section, places[p].section) != 0)
			return false;
		// Every place between the group's first and this one belongs to the group.
		for (int q = first; q < p; ++q)
		{
			if (!places[q].group || strcmp(places[q].group, places[p].group) != 0)
				return false;
		}
		if (first != p)
			continue;
		int m = ModuleNamed(places[p].module);
		int k = m >= 0 ? KeyNamed(kConfigModules[m], places[p].key) : -1;
		if (k < 0)
			return false;
		const ConfigKey& key = kConfigModules[m].keys[k];
		if (key.kind != CK_BOOL || !SettingsKeyShown(key, false))
			return false;
	}
	return true;
}

// 0 for a place without a group, else one plus the index of the group's first place.
static int GroupIdOf(const SettingsPlace* places, int p)
{
	return places[p].group ? GroupFirst(places, p) + 1 : 0;
}

void AddPlayerSections(const SettingsPlace* places, SettingsStaging* staging, std::vector<SettingsRow>* out)
{
	bool placed[CONFIG_MODULE_MAX][CONFIG_STAGE_MAX];
	memset(placed, 0, sizeof(placed));
	const bool groups = SettingsGroupsValid(places);

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
			SettingsKeyRef ref = { &kConfigModules[m], k, &staging->module[m], groups ? GroupIdOf(places, p) : 0 };
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
