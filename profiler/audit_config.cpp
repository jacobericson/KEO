// audit_config.cpp - Configuration loading.
// Startup thread; reads the INI without taking a probe lock.

#include "audit_detail.h"
#include "base/ini_names.h"
#include <stdlib.h>

namespace kenshiframeaudit_detail {

// =========================================================================
// Configuration loading
// =========================================================================

std::string IniPath()
{
	return g_dllDir + PROFILER_INI_NAME;
}

int IniInt(const char* key, int def)
{
	return (int)GetPrivateProfileIntA("Audit", key, def, IniPath().c_str());
}

std::string IniString(const char* key, const char* def)
{
	char buf[256];
	GetPrivateProfileStringA("Audit", key, def, buf, sizeof(buf), IniPath().c_str());
	return std::string(buf);
}

double IniDouble(const char* key, double def)
{
	std::string s = IniString(key, "");
	if (s.empty())
		return def;
	return atof(s.c_str());
}

double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
int    ClampI(int v, int lo, int hi)          { return v < lo ? lo : (v > hi ? hi : v); }

std::string Lower(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		char ch = s[i];
		if (ch == ' ' || ch == '\t')
			continue;
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		out += ch;
	}
	return out;
}

std::string SanitizeTag(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size() && out.size() < 32; ++i)
	{
		char ch = s[i];
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
			out += ch;
	}
	return out;
}

void LoadConfig()
{
	g_cfg.enabled                = IniInt("Enabled", 1) != 0;
	g_cfg.summarySec             = ClampD(IniDouble("SummarySec", 10.0), 1.0, 600.0);
	g_cfg.slowMs                 = ClampD(IniDouble("SlowMs", 33.3), 1.0, 60000.0);
	g_cfg.slowMax                = ClampI(IniInt("SlowMax", 30), 0, 100000);
	g_cfg.stallMs                = ClampD(IniDouble("StallMs", 1000.0), 50.0, 600000.0);
	g_cfg.csvSeconds             = IniInt("CsvSeconds", 1) != 0;
	g_cfg.csvFrames              = IniInt("CsvFrames", 0) != 0;
	g_cfg.tag                    = SanitizeTag(IniString("Tag", ""));
	g_cfg.boundaryRenderOneFrame = IniInt("FrameBoundary", 1) != 0;
	g_cfg.ogreSplit              = IniInt("OgreSplit", 1) != 0;
	g_cfg.draws                  = IniInt("Draws", 1) != 0;
	g_cfg.threadBodies           = IniInt("ThreadBodies", 1) != 0;
	g_cfg.aiVis                  = IniInt("AiVis", 1) != 0;
	g_cfg.aiLists                = IniInt("AiLists", 1) != 0;
	g_cfg.renderDetail           = IniInt("RenderDetail", 1) != 0;
	g_cfg.renderDeep             = IniInt("RenderDeep", 1) != 0;
	g_cfg.drawTop                = ClampI(IniInt("DrawTop", 12), 0, 100);
	g_cfg.particles              = IniInt("Particles", 1) != 0;
	g_cfg.particleTop            = ClampI(IniInt("ParticleTop", 20), 0, 100);
	g_cfg.physxDetail            = IniInt("PhysXDetail", 1) != 0;
	g_cfg.saveDetail             = IniInt("SaveDetail", 1) != 0;
	g_cfg.saveTop                = ClampI(IniInt("SaveTop", 10), 0, 96);
	g_cfg.disableSites           = Lower(IniString("DisableSites", ""));
	g_cfg.timerExperiment        = IniInt("TimerExperiment", 0) != 0;
	g_cfg.legacyFps              = IniInt("LegacyFps", 1) != 0;
	g_cfg.cursorSplitEvery       = ClampI(IniInt("CursorSplitEvery", 0), 0, 1000000);
	std::string charGroups       = IniString("CursorCharGroups", "");
	g_cfg.cursorCharGroups       = charGroups.empty() ? CHAR_GROUPS
	                                                  : (unsigned)strtoul(charGroups.c_str(), NULL, 0);
	g_cfg.listeners              = IniInt("Listeners", 1) != 0;
	g_cfg.hullDiag               = IniInt("HullDiag", 0) != 0;
	g_cfg.cpuSample              = IniInt("CpuSample", 1) != 0;
	g_cfg.steadyDetail           = IniInt("SteadyDetail", 0) != 0;
	g_cfg.offMainDetail          = IniInt("OffMainDetail", 0) != 0;
}

bool NameDisabled(const char* name)
{
	if (g_cfg.disableSites.empty())
		return false;
	std::string list = "," + g_cfg.disableSites + ",";
	std::string key  = "," + Lower(name) + ",";
	return list.find(key) != std::string::npos;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
