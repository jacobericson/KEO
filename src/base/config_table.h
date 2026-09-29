#pragma once
#include "base/ini_text.h"
#include <stddef.h>
#include <string>
#include <vector>

// The INI keys as data. The loader, the INI writer and the retired-key report
// walk these tables; no Windows or game header, so the host suites link them.

enum ConfigKind { CK_BOOL, CK_INT, CK_FLOAT, CK_DOUBLE, CK_TEXT, CK_CUSTOM };

typedef void (*ConfigLogFn)(const std::string& line);

// A custom row's parser: stores the value into its own storage, a module
// config field or the parser's own state. True counts the line as applied.
typedef bool (*ConfigParseFn)(const std::string& val, ConfigLogFn log);

// One legal value of a CK_INT or CK_CUSTOM row: the INI text, the value the
// field holds for it, and the settings page's name for it.
struct ConfigChoice
{
	const char* ini;
	int         value;
	const char* label;
};

struct ConfigKey
{
	const char*   name;
	ConfigKind    kind;
	size_t        offset;   // of the field in the module's state (offset rows)
	size_t        size;     // an offset row: its field's width; CK_TEXT: the buffer size; CK_CUSTOM: the target's width in bytes
	float         lo, hi;   // the clamp range; none when lo > hi
	bool          live;     // false: read at startup only, never applied at runtime
	// The settings panel's row. A NULL label keeps the key INI-only.
	const char*   label;
	const char*   tooltip;
	bool          devOnly;  // a diagnostic: shown in DEV builds only
	// CK_FLOAT: the slider drags over sliderLo + k / 2^stepExp up to hi (the
	// game's setPrecision takes a power-of-two exponent, not decimal digits).
	// sliderLo is chosen so the default and hi lie on that grid; the clamp
	// range lo..hi still governs typed values.
	float         sliderLo;
	int           stepExp;

	void*         target;             // the global a target row writes; NULL for an offset row
	int           minInt;             // CK_INT: a smaller value is refused (INT_MIN: none)
	bool          clampPositiveOnly;  // clamped only while the value is above 0
	bool          documented;         // the INI template has a line for it
	bool          retired;            // an old key: reported once, never applied
	const char*   devDefault;         // target rows: the default as the INI writes it
	const char*   prodDefault;
	ConfigParseFn parse;              // CK_CUSTOM
	const ConfigChoice* choices;
	int           choiceCount;
	bool          debugOnlyReader; // the code that reads this key is compiled only under ZONEOPT_DEBUG
};

// One table of keys and the state its offset rows live in. keys ends with a
// row whose name is NULL; every walk runs to it.
struct ConfigModule
{
	const char*      name;
	const char*      title;
	const ConfigKey* keys;
	void*            state;       // NULL for a module of target rows
	const void*      defaults;    // the state's compiled-in values, or NULL
	size_t           stateSize;   // sizeof(*state), 0 for a module of target rows
};

// Every module, in the settings page's order (config_keys.cpp).
extern const ConfigModule kConfigModules[];
extern const int kConfigModuleCount;

// The active (not retired) row named name, and its module; NULL when none.
const ConfigKey* FindConfigKey(const std::string& name, const ConfigModule** module);

// Parses val by the row's kind into its target or its offset in the
// module's state. False leaves the value untouched.
bool ConfigApplyValue(const ConfigModule& m, const ConfigKey& k, const std::string& val, ConfigLogFn log);

// The value as the INI writes it: a target row's from its global, an offset
// row's from state. A float reads back to the same bits. A CK_CUSTOM row
// prints its choice's INI text when it has choices, else its target as a bool
// (one byte wide) or an integer; "" for a row with nowhere to read.
std::string ConfigFormatValue(const ConfigModule& m, const ConfigKey& k, const void* state);

// One INI load's running counts.
struct ConfigLoadState
{
	int                     overrides;      // lines applied
	int                     unrecognised;   // unknown or invalid
	int                     retired;        // retired keys reported (once each)
	std::vector<IniDupSeen> dupSeen;        // keys applied so far, for the duplicate report
	std::vector<std::string> debugIgnored; // DEV-only readers ignored in PROD, once per key
	std::vector<std::string> retiredSeen;   // retired keys already reported

	ConfigLoadState() : overrides(0), unrecognised(0), retired(0) {}
};

// One key=value line: a table row (either module), then a bench slot key,
// then a retired row, then unknown; lineNo feeds the duplicate report.
void ConfigApplyLine(const std::string& key, const std::string& val, int lineNo, ConfigLoadState* st, ConfigLogFn log);

// The loader summary, empty when there is nothing to report.
std::string ConfigSummaryLine(const ConfigLoadState& st, int dupCount);

// The value rule for one CK_INT, CK_FLOAT or CK_DOUBLE row, which the loader
// and the settings commit both apply; p holds a value of the row's own type.
// An int below minInt is refused and left as it is. A value outside lo..hi
// is set to the bound, with "Config: <key>=<val> clamped to min|max <bound>"
// when log is not NULL; a clampPositiveOnly row is clamped only while above
// 0. A row without a range, or of another kind, is kept.
enum ConfigClampResult { CLAMP_KEPT, CLAMP_CHANGED, CLAMP_REFUSED };
ConfigClampResult ConfigClampValue(const ConfigKey& k, void* p, ConfigLogFn log);

// Clamps the loaded values in a fixed order through ConfigClampValue, one
// line per changed value.
void ConfigClampLoaded(ConfigLogFn log);

// The INI kind a row's value is written and compared as.
IniValueKind ConfigIniKind(ConfigKind kind);

// Whether an offset row's field holds the same value in states a and b.
bool ConfigOffsetValueEqual(const ConfigKey& k, const void* a, const void* b);

// Whether an entry with this value is added to a file without a line for its
// key: a target row's when value differs from its default for this build, an
// offset row's when state differs from defaults (always without defaults).
bool ConfigEntryAppends(const ConfigKey& k, const std::string& value, const void* state, const void* defaults);

// The module's keys as INI entries for the writer, one per row it can read.
// A target row is appended when its value differs from its default for this
// build; an offset row when it differs from defaults.
void ConfigIniEntries(const ConfigModule& m, const void* state, const void* defaults, std::vector<IniEntry>* out);
