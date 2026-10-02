// config_table.cpp — the functions over any config module: lookup, parse,
// format, the per-line load decision, the clamps and the writer's entries.

#include "base/config_table.h"
#include "bench/bench_slots.h"
#include <cstdlib>
#include <cstring>
#include <sstream>

static bool ParseDouble(const std::string& val, double* out)
{
	char* end = NULL;
	double d = strtod(val.c_str(), &end);
	if (end == val.c_str()) return false;
	*out = d;
	return true;
}

static void CopyText(char* dst, size_t size, const char* src)
{
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
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

// Where a row's value lives: its global, or its offset in state.
static char* ValueAt(const ConfigKey& k, const void* state)
{
	if (k.target)
		return (char*)k.target;
	if (state)
		return (char*)state + k.offset;
	return NULL;
}

// A CK_CUSTOM target's value, read at its width.
static int CustomValue(const ConfigKey& k, const char* p)
{
	if (k.size == 1)
		return *(const bool*)p ? 1 : 0;
	return *(const int*)p;
}

const ConfigKey* FindConfigKey(const std::string& name, const ConfigModule** module)
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigKey* keys = kConfigModules[m].keys;
		for (int i = 0; keys[i].name; ++i)
		{
			if (!keys[i].retired && name == keys[i].name)
			{
				if (module)
					*module = &kConfigModules[m];
				return &keys[i];
			}
		}
	}
	return NULL;
}

static const ConfigKey* FindRetiredKey(const std::string& name)
{
	for (int m = 0; m < kConfigModuleCount; ++m)
	{
		const ConfigKey* keys = kConfigModules[m].keys;
		for (int i = 0; keys[i].name; ++i)
		{
			if (keys[i].retired && name == keys[i].name)
				return &keys[i];
		}
	}
	return NULL;
}

// An int the loader refuses outright rather than clamps.
static bool IntRefused(const ConfigKey& k, int v)
{
	return v < k.minInt;
}

bool ConfigApplyValue(const ConfigModule& m, const ConfigKey& k, const std::string& val, ConfigLogFn log)
{
	if (k.kind == CK_CUSTOM)
		return k.parse ? k.parse(val, log) : false;
	char* p = ValueAt(k, m.state);
	if (!p)
		return false;
	switch (k.kind)
	{
	case CK_BOOL:
		return ParseBool(val, (bool*)p);
	case CK_INT:
	{
		int v;
		if (!ParseInt(val, &v) || IntRefused(k, v))
			return false;
		*(int*)p = v;
		return true;
	}
	case CK_FLOAT:
		return ParseFloat(val, (float*)p);
	case CK_DOUBLE:
		return ParseDouble(val, (double*)p);
	default:
		CopyText(p, k.size, val.c_str());
		return true;
	}
}

std::string ConfigFormatValue(const ConfigModule& m, const ConfigKey& k, const void* state)
{
	(void)m;
	const char* p = ValueAt(k, state);
	if (!p)
		return "";
	std::ostringstream ss;
	switch (k.kind)
	{
	case CK_BOOL:
		return *(const bool*)p ? "true" : "false";
	case CK_INT:
		ss << *(const int*)p;
		return ss.str();
	case CK_FLOAT:
		return FormatFloat(*(const float*)p);
	case CK_DOUBLE:
		ss << *(const double*)p;
		return ss.str();
	case CK_CUSTOM:
	{
		int v = CustomValue(k, p);
		for (int i = 0; i < k.choiceCount; ++i)
		{
			if (k.choices[i].value == v)
				return k.choices[i].ini;
		}
		if (k.size == 1)
			return v ? "true" : "false";
		ss << v;
		return ss.str();
	}
	default:
		return p;
	}
}

void ConfigApplyLine(const std::string& key, const std::string& val, int lineNo, ConfigLoadState* st, ConfigLogFn log)
{
	bool matched = false;
	const ConfigModule* m = NULL;
	const ConfigKey* k = FindConfigKey(key, &m);
	if (k)
		matched = ConfigApplyValue(*m, *k, val, log);
	else if (ParseBenchSlotKey(key, val, g_benchSlots))
		matched = true;

	if (matched)
	{
#ifndef KEO_DEBUG
		if (k && k->debugOnlyReader)
		{
			for (size_t i = 0; i < st->debugIgnored.size(); ++i)
				if (st->debugIgnored[i] == key) return;
			log("Config: DEV-only key '" + key + "' ignored in this build");
			st->debugIgnored.push_back(key);
			return;
		}
#endif
		st->overrides++;
		IniNoteAppliedKey(st->dupSeen, key, lineNo);
		return;
	}
	if (FindRetiredKey(key))
	{
		for (size_t i = 0; i < st->retiredSeen.size(); ++i)
		{
			if (st->retiredSeen[i] == key)
				return;
		}
		log("Config: retired key '" + key + "' ignored");
		st->retiredSeen.push_back(key);
		++st->retired;
		return;
	}
	log("Config: unknown or invalid key '" + key + "'");
	st->unrecognised++;
}

std::string ConfigSummaryLine(const ConfigLoadState& st, int dupCount)
{
	if (st.overrides <= 0 && st.unrecognised <= 0 && st.retired <= 0 && dupCount <= 0 && st.debugIgnored.empty())
		return "";
	std::ostringstream ss;
	ss << "Config: " << st.overrides << " setting(s) loaded from INI, "
	   << st.unrecognised << " unrecognised, " << st.retired << " retired, "
	   << "dup=" << dupCount;
	if (!st.debugIgnored.empty()) ss << ", " << st.debugIgnored.size() << " DEV-only ignored";
	if (st.unrecognised > 0)
		ss << " (unrecognised listed above, left at their defaults)";
	return ss.str();
}

// The clamp order the log has always shown.
static const char* const kClampOrder[] =
{
	"preloadKeepAliveSeconds",
	"navmeshWorkerCount",
	"navmeshGenConcurrency",
	"navmeshDiskCacheMaxMB",
	"camLogInterval",
	"reprioritizeInterval",
	"camFocusMaxDist",
	"camFocusHardMult",
	"camFocusHysteresis",
	"zoneLifeRetainRadius",
	"islandFarSpan",
	"zoneLifeIdleSeconds",
	NULL
};

// Clamps *v into lo..hi, logging the value and the bound as T.
template <typename T>
static ConfigClampResult ClampLogged(const char* name, T* v, T lo, T hi, ConfigLogFn log)
{
	if (*v < lo)
	{
		std::ostringstream ss;
		ss << "Config: " << name << "=" << *v << " clamped to min " << lo;
		if (log)
			log(ss.str());
		*v = lo;
		return CLAMP_CHANGED;
	}
	if (*v > hi)
	{
		std::ostringstream ss;
		ss << "Config: " << name << "=" << *v << " clamped to max " << hi;
		if (log)
			log(ss.str());
		*v = hi;
		return CLAMP_CHANGED;
	}
	return CLAMP_KEPT;
}

ConfigClampResult ConfigClampValue(const ConfigKey& k, void* p, ConfigLogFn log)
{
	if (k.kind == CK_INT && IntRefused(k, *(int*)p))
		return CLAMP_REFUSED;
	if (k.lo > k.hi)
		return CLAMP_KEPT;
	switch (k.kind)
	{
	case CK_FLOAT:
		if (k.clampPositiveOnly && !(*(float*)p > 0.0f))
			return CLAMP_KEPT;
		return ClampLogged<float>(k.name, (float*)p, k.lo, k.hi, log);
	case CK_DOUBLE:
		return ClampLogged<double>(k.name, (double*)p, (double)k.lo, (double)k.hi, log);
	case CK_INT:
		return ClampLogged<int>(k.name, (int*)p, (int)k.lo, (int)k.hi, log);
	default:
		return CLAMP_KEPT;
	}
}

void ConfigClampLoaded(ConfigLogFn log)
{
	for (int i = 0; kClampOrder[i]; ++i)
	{
		const ConfigModule* m = NULL;
		const ConfigKey* k = FindConfigKey(kClampOrder[i], &m);
		if (!k || k->lo > k->hi)
			continue;
		char* p = ValueAt(*k, m->state);
		if (p)
			ConfigClampValue(*k, p, log);
	}
}

IniValueKind ConfigIniKind(ConfigKind kind)
{
	switch (kind)
	{
	case CK_BOOL:   return INI_BOOL;
	case CK_INT:    return INI_INT;
	case CK_FLOAT:
	case CK_DOUBLE: return INI_FLOAT;
	default:        return INI_TEXT;
	}
}

static size_t ValueWidth(const ConfigKey& k)
{
	switch (k.kind)
	{
	case CK_BOOL:   return sizeof(bool);
	case CK_INT:    return sizeof(int);
	case CK_FLOAT:  return sizeof(float);
	case CK_DOUBLE: return sizeof(double);
	default:        return k.size;
	}
}

bool ConfigOffsetValueEqual(const ConfigKey& k, const void* a, const void* b)
{
	const char* pa = (const char*)a + k.offset;
	const char* pb = (const char*)b + k.offset;
	if (k.kind == CK_BOOL)
		return *(const bool*)pa == *(const bool*)pb;
	if (k.kind == CK_TEXT)
		return strcmp(pa, pb) == 0;
	return memcmp(pa, pb, ValueWidth(k)) == 0;
}

bool ConfigEntryAppends(const ConfigKey& k, const std::string& value, const void* state, const void* defaults)
{
	if (k.target)
	{
#ifdef KEO_DEBUG
		const char* def = k.devDefault;
#else
		const char* def = k.prodDefault;
#endif
		return !def || !IniValueEquals(ConfigIniKind(k.kind), def, value);
	}
	return !defaults || !ConfigOffsetValueEqual(k, state, defaults);
}

void ConfigIniEntries(const ConfigModule& m, const void* state, const void* defaults, std::vector<IniEntry>* out)
{
	for (int i = 0; m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		if (k.retired || !ValueAt(k, state))
			continue;
		// A custom row without choices has no INI text for its value.
		if (k.kind == CK_CUSTOM && !k.choices)
			continue;
		IniEntry e;
		e.key = k.name;
		e.value = ConfigFormatValue(m, k, state);
		e.kind = ConfigIniKind(k.kind);
		e.append = ConfigEntryAppends(k, e.value, state, defaults);
		out->push_back(e);
	}
}
