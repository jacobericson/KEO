#include "base/config.h"
#include "base/config_table.h"
#include "base/worker_count.h"
#include "base/hash.h"
#include "base/ini_names.h"
#include "base/legacy_ini_import.h"
#include "render/render_config.h"
#include <cstdio>
#include <cstring>


// =========================================================================
// Active mod set hash
// =========================================================================
//
// mods.cfg (under the game's data folder) lists the active mods in load order,
// one per line.
// Mods can move terrain and buildings, so a changed mod set must not reuse the
// navmesh meshes of the old one. The hash goes into every L2 filename, which
// also keeps two mod sets' caches side by side instead of fighting over the
// same names. Read once here, on the main thread at startup.
//
// The game's own list (GameWorld::activeMods, lektor<ModInfo*> at +0x528) is
// not used: it needs a GameWorld pointer and is not populated this early.
static unsigned int ComputeModSetHash(std::string& usedPath)
{
	char exePath[MAX_PATH];
	char path[MAX_PATH];
	FILE* f = NULL;

	usedPath.clear();

	// Candidates in order. Kenshi keeps the file at <game root>\data\mods.cfg;
	// a copy in the game root itself does not exist on every install, so that
	// is only the second try. Both are then repeated relative to the DLL, which
	// lives at <game root>\mods\<folder>\, for launchers that start the
	// executable from somewhere else.
	std::string candidates[4];
	int nCand = 0;

	// _TRUNCATE keeps an over-long path from reaching the secure CRT's
	// invalid-parameter handler, which would terminate the process; it returns
	// -1 instead and the next candidate takes over.
	DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
	{
		char* slash = strrchr(exePath, '\\');
		if (slash)
		{
			slash[1] = 0;
			if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%sdata\\mods.cfg", exePath) >= 0)
				candidates[nCand++] = path;
			if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%smods.cfg", exePath) >= 0)
				candidates[nCand++] = path;
		}
	}

	std::string dllDir = GetDLLDirectory();
	candidates[nCand++] = dllDir + "..\\..\\data\\mods.cfg";
	candidates[nCand++] = dllDir + "..\\..\\mods.cfg";

	for (int i = 0; i < nCand && !f; ++i)
	{
		fopen_s(&f, candidates[i].c_str(), "rb");
		if (f)
			usedPath = candidates[i];
	}

	if (!f)
		return 0;

	unsigned int h = FNV1A32_OFFSET;
	unsigned char buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		for (size_t i = 0; i < got; ++i)
		{
			if (buf[i] == '\r') continue;   // normalize line endings
			h = Fnv1a32Mix(h, buf[i]);
		}
	}
	fclose(f);

	if (h == 0) h = 1;   // 0 is reserved for "mod list unavailable"
	return h;
}


// =========================================================================
// LoadConfig — reads KEO.ini, applies values with validation
// =========================================================================

void LoadConfig(const std::string& dllDir)
{
	std::string modsCfgPath;
	navmesh::g_navmeshCfg.g_modSetHash = ComputeModSetHash(modsCfgPath);
	{
		std::ostringstream ss;
		ss << "Active mod set hash: " << std::hex << navmesh::g_navmeshCfg.g_modSetHash << std::dec;
		if (navmesh::g_navmeshCfg.g_modSetHash == 0)
			ss << " (mods.cfg not found — L2 cache keyed without it)";
		else
			ss << " (from " << modsCfgPath << ")";
		LogMsg(ss.str());
	}

	LegacyIniImport(dllDir, LEGACY_OPTIMIZER_INI_NAME, OPTIMIZER_INI_NAME, LEGACY_OPTIMIZER_RETIRE_NAME, true, &LogMsg);

	std::string iniPath = dllDir + OPTIMIZER_INI_NAME;
	FILE* f = NULL;
	fopen_s(&f, iniPath.c_str(), "r");
	if (!f)
	{
		LogMsg("No INI file found, using defaults");
		FinalizeConfig();
		return;
	}

	LogMsg("Loading config from " + iniPath);
	ConfigLoadState st;   // counts, retired keys reported, keys applied (for the duplicate report)

	// A key=value line normally fits in one fgets() call; lineNo only advances
	// when the previous read actually ended the physical line, so a line
	// longer than the buffer still counts as one line instead of several. A
	// read that continues a line is skipped (it would parse as a line of its
	// own), with one line when it holds more than whitespace.
	int lineNo = 0;
	int longLogged = 0;
	bool atLineStart = true;
	char lineBuf[512];
	while (fgets(lineBuf, sizeof(lineBuf), f))
	{
		bool continued = !atLineStart;
		if (atLineStart)
			++lineNo;
		size_t rawLen = strlen(lineBuf);
		atLineStart = (rawLen > 0 && lineBuf[rawLen - 1] == '\n');
		if (continued)
		{
			if (longLogged != lineNo && strspn(lineBuf, " \t\r\n") != rawLen)
			{
				char msg[96];
				_snprintf_s(msg, sizeof(msg), _TRUNCATE,
				            "Config: line %d is longer than 511 characters, the rest of it is ignored", lineNo);
				LogMsg(msg);
				longLogged = lineNo;
			}
			continue;
		}

		std::string key, val;
		if (!SplitIniLine(std::string(lineBuf), &key, &val))
			continue;

		ConfigApplyLine(key, val, lineNo, &st, &LogMsg);
	}

	fclose(f);

	// Duplicate keys: the loader above applies the last value; log one line per
	// repeated key so the log states which value was actually used.
	int dupCount = 0;
	for (size_t i = 0; i < st.dupSeen.size(); ++i)
	{
		if (st.dupSeen[i].firstLine != st.dupSeen[i].lastLine)
		{
			LogMsg(IniDupMessage(st.dupSeen[i]));
			++dupCount;
		}
	}

	// --- Validate and clamp ---
	ConfigClampLoaded(&LogMsg);

	// Always reported, default included: it changes when the game unloads the
	// zones the mod preloads, so any log read against a crash needs to state it.
	{
		std::ostringstream ss;
		ss << "Config: preloadKeepAliveSeconds=" << zone::g_zoneCfg.cfg_preloadKeepAliveSeconds
		   << (zone::g_zoneCfg.cfg_preloadKeepAliveSeconds > 0.0f
		       ? " (overrides the game's per-timer default)"
		       : " (the game's per-timer default)");
#if ZONEHAND_STEP >= 2
		// ZoneHandoffNoteLoaded clears all three countdowns right after a mod
		// load applies this value (zone_handoff.cpp), so it never survives to
		// take effect at this step.
		ss << " (inert at ZONEHAND_STEP>=2)";
#endif
		LogMsg(ss.str());
	}

	// Always reported too: it decides how every navmesh the mod generates is
	// pruned, and which L2 settings hash the cache uses.
	{
		std::ostringstream ss;
		ss << "Config: navmeshVanillaPruning=" << (navmesh::g_navmeshCfg.navmeshVanillaPruningEnabled ? "true" : "false")
		   << " (NMPRUNE_STEP " << 2;
		if (!navmesh::g_navmeshCfg.navmeshVanillaPruningEnabled)
			ss << ": off, fresh work buffers keep Havok's pruning defaults)";
		else
			ss << ": fresh work buffers carry the game's region pruning and extra-vertex settings)";
		LogMsg(ss.str());
	}

	// Always reported too: it decides the neighbour seeds of every navmesh
	// the mod generates, and which L2 settings hash the cache uses.
	{
		std::ostringstream ss;
		ss << "Config: navmeshNeighbourSeeds=" << (navmesh::g_navmeshCfg.navmeshNeighbourSeedsEnabled ? "true" : "false")
		   << " (NMNBRSEED_STEP " << 2;
		if (!navmesh::g_navmeshCfg.navmeshNeighbourSeedsEnabled)
			ss << ": off, no neighbour-seed hook, no stand-in seeds)";
		else
			ss << ": a missing or temp neighbour contributes its shipped tile's border seeds)";
		LogMsg(ss.str());
	}

	// The per-key lines above scroll past in a file this size, and a key the
	// loader rejected leaves the setting at its default while the INI reads as
	// though it took. The count travels with the summary so the two are read
	// together.
	std::string summary = ConfigSummaryLine(st, dupCount);
	if (!summary.empty()) LogMsg(summary);

	FinalizeConfig();
}


// =========================================================================
// FinalizeConfig — work that must run on every LoadConfig path, whether or
// not an INI file was found
// =========================================================================

void FinalizeConfig()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	int cpus = (int)si.dwNumberOfProcessors;
	navmesh::g_navmeshCfg.g_navMeshWorkerCount = navmesh::g_navmeshCfg.cfg_navmeshWorkerCount ? navmesh::g_navmeshCfg.cfg_navmeshWorkerCount
	                                              : AutoNavMeshWorkerCount(cpus, NAVMESH_WORKER_COUNT);
	std::ostringstream ss;
	ss << "Config: navmeshWorkerCount=" << (navmesh::g_navmeshCfg.cfg_navmeshWorkerCount ? "" : "auto ")
	   << navmesh::g_navmeshCfg.g_navMeshWorkerCount << " (cpus=" << cpus << ", capacity " << NAVMESH_WORKER_COUNT << ")";
	LogMsg(ss.str());

	ClampRenderConfig();
	LogRenderConfig();
}
