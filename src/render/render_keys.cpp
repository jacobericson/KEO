#include "render/render_keys.h"
#include "base/ini_text.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <float.h>
#include <limits.h>
#include <sstream>

#define RK_FIELD(f)      offsetof(RenderConfig, f)
#define RK_FIELD_SIZE(f) sizeof(((RenderConfig*)0)->f)
// A render row: its twelve values as written, then the columns a render row
// never uses (offset rows have no target, no integer floor, no row defaults).
#define RK_ROW(name, kind, offset, size, lo, hi, live, label, tooltip, devOnly, sliderLo, stepExp) \
	{ name, kind, offset, size, lo, hi, live, label, tooltip, devOnly, sliderLo, stepExp, \
	  NULL, INT_MIN, false, true, false, NULL, NULL, NULL, NULL, 0 }

const RenderKey g_renderKeys[] =
{
	RK_ROW("renderLevers",             RK_BOOL,  RK_FIELD(renderLevers),             0, 0.0f, 0.0f, false,
	  "Render and particle levers",
	  "Master switch for every render and particle lever, including those set only in KEO.ini. Read when the game starts.", false, 0.0f, 0),
	RK_ROW("reflectionHalfRate",       RK_BOOL,  RK_FIELD(reflectionHalfRate),       0, 0.0f, 0.0f, true,
	  "Water reflection at half rate",
	  "Renders the water reflection every other frame.", true, 0.0f, 0),
	RK_ROW("shadowReachDiag",          RK_BOOL,  RK_FIELD(shadowReachDiag),          0, 0.0f, 0.0f, true,
	  "Shadow reach counter (DEV)",
	  "Counts the shadow casters that can reach a lit pixel, on the Render: log line.", true, 0.0f, 0),
	RK_ROW("particleOffscreenSkip",    RK_BOOL,  RK_FIELD(particleOffscreenSkip),    0, 0.0f, 0.0f, true,
	  "Pause off-screen particle effects",
	  "Looping effects out of view stop updating after the off-screen time below.", true, 0.0f, 0),
	RK_ROW("particleStepCap",          RK_BOOL,  RK_FIELD(particleStepCap),          0, 0.0f, 0.0f, true,
	  "Cap particle steps at high game speed",
	  "Above a game speed set by particleStepCapSpeed in KEO.ini (3x by default), particle effects advance in capped steps, so smoke and fire animate slower.", false, 0.0f, 0),
	RK_ROW("gpuParamLookupDiag",       RK_BOOL,  RK_FIELD(gpuParamLookupDiag),       0, 0.0f, 0.0f, true,
	  "Shader constant lookup counter (DEV)",
	  "Counts shader constant lookups per frame, on the Render: log line.", true, 0.0f, 0),
	RK_ROW("renderDiag",               RK_BOOL,  RK_FIELD(renderDiag),               0, 0.0f, 0.0f, true,
	  "Render stats in the log",
	  "Writes the Render: line to KEO.log every 30 seconds.", true, 0.0f, 0),
	RK_ROW("particleStepCapSpeed",     RK_FLOAT, RK_FIELD(particleStepCapSpeed),     0, 1.5f, 20.0f, true,
	  "Particle step cap from game speed",
	  "The game speed above which the particle step cap applies.", true, 1.5f, 1),
	RK_ROW("particleOffscreenSeconds", RK_FLOAT, RK_FIELD(particleOffscreenSeconds), 0, 0.1f, 10.0f, true,
	  "Off-screen time before pausing (s)",
	  "How long a looping effect must be out of view before it stops updating.", true, 0.125f, 3),
	RK_ROW("particleOffscreenMinAge",  RK_FLOAT, RK_FIELD(particleOffscreenMinAge),  0, 0.0f, 120.0f, true,
	  "Minimum effect age to pause (s)",
	  "Effects younger than this, in game seconds, are never paused.", true, 0.0f, 0),
	RK_ROW("particleLoopingNames",     RK_TEXT,  RK_FIELD(particleLoopingNames),
	  RK_FIELD_SIZE(particleLoopingNames), 0.0f, 0.0f, true, NULL, NULL, false, 0.0f, 0),
	RK_ROW("oldAnimSkip",              RK_BOOL,  RK_FIELD(oldAnimSkip),              0, 0.0f, 0.0f, true,
	  "Skip idle character animation passes",
	  "Skips a render pass's character animation update when no visible character needs one.", true, 0.0f, 0),
	RK_ROW("oldAnimDiag",              RK_BOOL,  RK_FIELD(oldAnimDiag),              0, 0.0f, 0.0f, true,
	  "Animation pass counter (DEV)",
	  "Counts the animation passes that could be skipped and times the check, on the Render: log line.", true, 0.0f, 0),
	RK_ROW("gpuParamCache",            RK_BOOL,  RK_FIELD(gpuParamCache),            0, 0.0f, 0.0f, true,
	  "Cache shader constant lookups",
	  "Remembers where each shader constant lives instead of searching for it on every draw.", true, 0.0f, 0),
	RK_ROW("shadowReachCull",          RK_BOOL,  RK_FIELD(shadowReachCull),          0, 0.0f, 0.0f, true,
	  "Cull unreachable shadow casters",
	  "Leaves out of each shadow cascade the casters that cannot shadow any pixel it lights.", true, 0.0f, 0),
	RK_ROW("emptyPassSkip",            RK_BOOL,  RK_FIELD(emptyPassSkip),            0, 0.0f, 0.0f, true,
	  "Skip empty debug and interior-mask passes",
	  "Turns the debug and interior-mask render passes off while they would draw nothing: "
	  "their queues are empty, or nothing in them is shown and in view. "
	  "Fog fades keep their usual speed.", true, 0.0f, 0),
	RK_ROW("foliagePageBudgetMs",      RK_FLOAT, RK_FIELD(foliagePageBudgetMs),      0, 0.0f, 50.0f, true,
	  "Foliage build budget at speed (ms, 0 = off)",
	  "Above a game speed set by foliageBudgetSpeed in KEO.ini (3x by default), foliage stops building pages for the rest of a frame once this many milliseconds are spent; trees fill in later.", false, 0.0f, 1),
	RK_ROW("foliageBudgetSpeed",       RK_FLOAT, RK_FIELD(foliageBudgetSpeed),       0, 1.5f, 20.0f, true,
	  "Foliage budget from game speed",
	  "The game speed above which the foliage build budget applies.", true, 1.5f, 1),
	RK_ROW("gpuUploadDiag",            RK_BOOL,  RK_FIELD(gpuUploadDiag),            0, 0.0f, 0.0f, true,
	  "Constant upload counter (DEV)",
	  "Counts shader constant uploads identical to the buffer's previous one, on the Render: log line.", true, 0.0f, 0),
	RK_ROW("gpuUploadSkip",            RK_BOOL,  RK_FIELD(gpuUploadSkip),            0, 0.0f, 0.0f, true,
	  "Skip unchanged shader constant uploads",
	  "Leaves a shader's constant buffer as it is when a draw would upload the same bytes it already holds.", true, 0.0f, 0),
	RK_ROW("adoptOgrePurgeSkip",       RK_BOOL,  RK_FIELD(adoptOgrePurgeSkip),       0, 0.0f, 0.0f, true,
	  "Skip the resource purge in a background zone adoption cycle",
	  "Leaves out the per-cycle Ogre resource purge while a loading cycle was raised by the zone handoff mechanism and no real transition joined it. A real transition's purge is never affected.", true, 0.0f, 0),
	RK_ROW("adoptOgrePurgeMaxSkipSeconds", RK_FLOAT, RK_FIELD(adoptOgrePurgeMaxSkipSeconds), 0, 0.0f, 3600.0f, true,
	  "Ogre purge fallback timeout (s)",
	  "Forces the purge to run once after it has been skipped this long, bounding how much unreferenced Ogre content can pile up.", true, 0.0f, 0),
	RK_ROW(NULL, RK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0)
};

static bool* BoolAt(RenderConfig* cfg, const RenderKey& k) { return (bool*)((char*)cfg + k.offset); }
static float* FloatAt(RenderConfig* cfg, const RenderKey& k) { return (float*)((char*)cfg + k.offset); }
static char* TextAt(RenderConfig* cfg, const RenderKey& k) { return (char*)cfg + k.offset; }
static const bool* BoolAt(const RenderConfig& cfg, const RenderKey& k) { return (const bool*)((const char*)&cfg + k.offset); }
static const float* FloatAt(const RenderConfig& cfg, const RenderKey& k) { return (const float*)((const char*)&cfg + k.offset); }
static const char* TextAt(const RenderConfig& cfg, const RenderKey& k) { return (const char*)&cfg + k.offset; }

const RenderKey* FindRenderKey(const std::string& name)
{
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		if (name == g_renderKeys[i].name)
			return &g_renderKeys[i];
	}
	return NULL;
}

static void CopyText(char* dst, size_t size, const char* src)
{
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

RenderParse ParseRenderKey(RenderConfig* cfg, const std::string& key, const std::string& val)
{
	const RenderKey* k = FindRenderKey(key);
	if (!k)
		return RP_NOT_RENDER;
	switch (k->kind)
	{
	case RK_BOOL:
		return ParseBool(val, BoolAt(cfg, *k)) ? RP_OK : RP_BAD_VALUE;
	case RK_FLOAT:
		return ParseFloat(val, FloatAt(cfg, *k)) ? RP_OK : RP_BAD_VALUE;
	default:
		CopyText(TextAt(cfg, *k), k->size, val.c_str());
		return RP_OK;
	}
}

// Six significant digits when they read back to the same float, else nine.
static std::string FormatFloat(float v)
{
	std::ostringstream ss;
	ss << v;
	if ((float)strtod(ss.str().c_str(), NULL) == v)
		return ss.str();
	std::ostringstream exact;
	exact.precision(9);
	exact << v;
	return exact.str();
}

std::string FormatRenderValue(const RenderConfig& cfg, const RenderKey& key)
{
	switch (key.kind)
	{
	case RK_BOOL:  return *BoolAt(cfg, key) ? "true" : "false";
	case RK_FLOAT: return FormatFloat(*FloatAt(cfg, key));
	default:       return TextAt(cfg, key);
	}
}

bool RenderValueEqual(const RenderConfig& a, const RenderConfig& b, const RenderKey& key)
{
	switch (key.kind)
	{
	case RK_BOOL:  return *BoolAt(a, key) == *BoolAt(b, key);
	case RK_FLOAT: return memcmp(FloatAt(a, key), FloatAt(b, key), sizeof(float)) == 0;
	default:       return strcmp(TextAt(a, key), TextAt(b, key)) == 0;
	}
}

void CopyRenderValue(RenderConfig* dst, const RenderConfig& src, const RenderKey& key)
{
	switch (key.kind)
	{
	case RK_BOOL:  *BoolAt(dst, key) = *BoolAt(src, key); break;
	case RK_FLOAT: *FloatAt(dst, key) = *FloatAt(src, key); break;
	default:       CopyText(TextAt(dst, key), key.size, TextAt(src, key)); break;
	}
}

void ClampRenderValues(RenderConfig* cfg, const RenderConfig& fallback, std::vector<std::string>* notes)
{
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (k.kind != RK_FLOAT)
			continue;
		float* v = FloatAt(cfg, k);
		if (!_finite(*v))
		{
			std::ostringstream ss;
			ss << k.name << "=" << *v << " is not a finite number, kept " << FormatFloat(*FloatAt(fallback, k));
			notes->push_back(ss.str());
			*v = *FloatAt(fallback, k);
		}
		float out = *v < k.lo ? k.lo : (*v > k.hi ? k.hi : *v);
		if (out == *v)
			continue;
		std::ostringstream ss;
		ss << k.name << "=" << *v << " clamped to " << (*v < k.lo ? "min " : "max ") << out;
		notes->push_back(ss.str());
		*v = out;
	}
}

void DiffRenderConfig(const RenderConfig& live, const RenderConfig& next, std::vector<RenderChange>* out)
{
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		if (RenderValueEqual(live, next, k))
			continue;
		RenderChange c;
		c.key = &k;
		c.before = FormatRenderValue(live, k);
		c.after = FormatRenderValue(next, k);
		out->push_back(c);
	}
}

std::string FormatRenderConfig(const RenderConfig& cfg)
{
	std::string s;
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		if (i)
			s += " ";
		s += g_renderKeys[i].name;
		s += "=";
		s += FormatRenderValue(cfg, g_renderKeys[i]);
	}
	return s;
}

static IniValueKind IniKind(RenderKeyKind kind)
{
	switch (kind)
	{
	case RK_BOOL:  return INI_BOOL;
	case RK_FLOAT: return INI_FLOAT;
	default:       return INI_TEXT;
	}
}

void RenderIniEntries(const RenderConfig& cfg, const RenderConfig& defaults, std::vector<IniEntry>* out)
{
	for (int i = 0; g_renderKeys[i].name; ++i)
	{
		const RenderKey& k = g_renderKeys[i];
		IniEntry e;
		e.key = k.name;
		e.value = FormatRenderValue(cfg, k);
		e.kind = IniKind(k.kind);
		e.append = !RenderValueEqual(cfg, defaults, k);
		out->push_back(e);
	}
}

std::string RewriteRenderIni(const std::string& text, const RenderConfig& cfg, const RenderConfig& defaults)
{
	std::vector<IniEntry> entries;
	RenderIniEntries(cfg, defaults, &entries);
	return RewriteIniKeys(text, entries, "[Render]");
}
