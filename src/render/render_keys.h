#pragma once
#include "render/render_config.h"
#include "base/config_table.h"
#include "base/ini_text.h"
#include <string>
#include <vector>

// The render keys as data: parse, clamp, diff, format and the INI rewrite all
// walk this one table. No Windows calls, so the host tests link it directly.
// A new render setting is a RenderConfig field plus one row here.

// A render key is a config row whose value lives at its offset in RenderConfig;
// the render module (config_keys.cpp) is this table over g_renderCfg.
typedef ConfigKind RenderKeyKind;
const RenderKeyKind RK_BOOL  = CK_BOOL;
const RenderKeyKind RK_FLOAT = CK_FLOAT;
const RenderKeyKind RK_TEXT  = CK_TEXT;

typedef ConfigKey RenderKey;

// In log order; a NULL name ends the table.
extern const RenderKey g_renderKeys[];

const RenderKey* FindRenderKey(const std::string& name);

enum RenderParse { RP_NOT_RENDER, RP_OK, RP_BAD_VALUE };
// Stores val into cfg's field for key; RP_BAD_VALUE leaves the field untouched.
RenderParse ParseRenderKey(RenderConfig* cfg, const std::string& key, const std::string& val);

// The value as the log and the INI write it; floats read back to the same bits.
std::string FormatRenderValue(const RenderConfig& cfg, const RenderKey& key);
bool RenderValueEqual(const RenderConfig& a, const RenderConfig& b, const RenderKey& key);
// One field, one store (RK_TEXT: a bounded copy).
void CopyRenderValue(RenderConfig* dst, const RenderConfig& src, const RenderKey& key);

// Clamps every float into its range. A non-finite float (NaN, inf) takes
// fallback's value first. One note per changed value: "<key>=<val> clamped
// to min|max <out>" or "<key>=<val> is not a finite number, kept <fallback>".
void ClampRenderValues(RenderConfig* cfg, const RenderConfig& fallback, std::vector<std::string>* notes);

struct RenderChange
{
	const RenderKey* key;
	std::string      before, after;
};
// Every key whose value differs, in table order.
void DiffRenderConfig(const RenderConfig& live, const RenderConfig& next, std::vector<RenderChange>* out);

// "renderLevers=true reflectionHalfRate=true ... particleLoopingNames=..."
std::string FormatRenderConfig(const RenderConfig& cfg);

// cfg's render keys as INI entries. A key is appended when the file has no
// line for it only if its value differs from defaults, so a later change of
// a default still reaches a user who saved once.
void RenderIniEntries(const RenderConfig& cfg, const RenderConfig& defaults, std::vector<IniEntry>* out);
// RewriteIniKeys over RenderIniEntries, appending into a [Render] section
// when the text has one.
std::string RewriteRenderIni(const std::string& text, const RenderConfig& cfg, const RenderConfig& defaults);
