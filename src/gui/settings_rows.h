#pragma once
#include "render/render_config.h"
#include "bench/bench_slots.h"
#include "base/ini_text.h"
#include "gui/settings_factory.h"
#include <string>
#include <utility>
#include <vector>

// The settings tab's content as data: which rows it shows, what each row is
// bound to, and what a close changes. No game or Windows calls, so the host
// tests link it directly.

// What the tab's rows edit. A row writes its bound field on every click, so
// the rows point here, never at the live config. module[m] is the staged
// copy of kConfigModules[m].
struct SettingsStaging
{
	ConfigModuleStage module[CONFIG_MODULE_MAX];
	int               benchSpeed[BENCH_SLOT_COUNT];   // 1 or 20; stored in a slot by Record or Run
};

// The index in kConfigModules[] of the module named "render", -1 when none
// is; a caller skips the render module's own steps on -1.
int RenderModuleIndex();

// The render module's staged state. With no render module it is a scratch
// copy that nothing stages or saves.
RenderConfig& StagedRender(SettingsStaging* s);

// SR_NOTE: a dim line after the last section, set apart by a space.
enum SettingsRowKind { SR_HEADER, SR_TEXT, SR_NOTE, SR_CHECKBOX, SR_SLIDER, SR_DROPBOX, SR_BUTTON };

// Button ids: the slot index plus RECORD or RUN; SWEEP alone.
enum { BENCH_BUTTON_RECORD = 100, BENCH_BUTTON_RUN = 200, BENCH_BUTTON_SWEEP = 300 };

struct SettingsRow
{
	SettingsRowKind kind;
	std::string     label;
	std::string     tooltip;
	bool*           boolPtr;    // SR_CHECKBOX
	float*          floatPtr;   // SR_SLIDER
	int*            intPtr;     // SR_DROPBOX
	float           lo, hi;     // SR_SLIDER drag range
	int             stepExp;    // SR_SLIDER: drags in steps of 2^-stepExp from lo
	std::vector<std::pair<std::string, int> > choices;   // SR_DROPBOX: text, value
	std::string     caption;    // SR_BUTTON: the button's text (label is the line's key, shown beside it)
	int             buttonId;   // SR_BUTTON
	bool            restart;    // a startup-only key's row, marked RESTART_MARK
};

// A startup-only key's label ends with RESTART_MARK and its tooltip with
// RESTART_NOTE; the footnote is "*" and RESTART_NOTE.
extern const char* const RESTART_MARK;
extern const char* const RESTART_NOTE;

// What the Benchmark section shows, read by the caller from the runner.
struct SettingsBench
{
	SettingsBench()
		: available(false), slots(NULL), activeSlot(-1), sweepLeg(0), sweepLegs(0), runSec(0.0), runSecCombined(0.0),
		  sweepLegCount(0) {}

	bool             available;
	std::string      reason;       // why not, when !available
	const BenchSlot* slots;        // BENCH_SLOT_COUNT entries
	int              activeSlot;   // the slot whose run is active, else -1
	int              sweepLeg;     // the sweep's leg in progress (1-based), else 0
	int              sweepLegs;    // the sweep's leg count while one runs
	double           runSec;           // one run with the current bench.levers; 0 leaves durations out
	double           runSecCombined;   // one run with bench.levers=combined
	int              sweepLegCount;    // legs in bench.sweep
};

// The sweep button's caption: "Full sweep", or "Sweep <leg>/<legs> (stop)"
// while one runs.
std::string SweepCaption(int leg, int legs);

// The rows in display order: with devBuild, each module's section in
// kConfigModules[] order, DEV-only keys included; without it, the player
// sections (AddPlayerSections). Each row is bound into its module's staged
// copy and ordered in its section by AddSectionRows; startup-only ones are
// marked RESTART_MARK. Then the RESTART_NOTE footnote when a marked row
// shows, then, when devBuild holds and bench is set, the Benchmark section:
// four rows per slot, then the sweep button. Every row's label is unique: the
// panel keys its lines by label.
void BuildSettingsRows(SettingsStaging* staging, bool devBuild, const SettingsBench* bench,
                       std::vector<SettingsRow>* out);

// staging->benchSpeed from the slots; 1 for a slot without a valid speed.
void StageBenchSpeeds(SettingsStaging* staging, const BenchSlot* slots);

struct SettingsDiff
{
	int applied;   // live render keys and live module rows that differ from the running config
	int saved;     // keys of every module that differ from the INI
};
SettingsDiff DiffSettings(const SettingsStaging& staged, const RenderConfig& live,
                          const SettingsStaging& saved);

// A close's clamp, before the diff: every module but the render one by the
// loader's rule (ClampModuleStage, lines to log), the render state by
// ClampRenderValues against fallback (lines to renderNotes).
void ClampSettings(SettingsStaging* staged, const SettingsStaging& saved, const RenderConfig& fallback,
                   ConfigLogFn log, std::vector<std::string>* renderNotes);
