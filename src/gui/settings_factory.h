#pragma once
#include "base/config_table.h"
#include "base/ini_text.h"
#include <vector>

// The settings tab's rows for one config module, built from its key table,
// and the module's staged copy the rows edit. No game or Windows calls, so
// the host tests link it directly.

struct SettingsRow;

// One slot-bound row's staged value (a target row, or an offset row into the
// staged module config): b for a checkbox, i for a drop box, f for a slider
// (an integer row's slider included).
struct ConfigStageValue
{
	bool  b;
	int   i;
	float f;
};

const int CONFIG_MODULE_MAX = 8;
const int CONFIG_STAGE_MAX  = 128;    // rows per module table, retired rows included
const int CONFIG_STATE_MAX  = 1024;   // bytes of one module's state struct

// One module's staged copy. Offset fields live in state (the double array
// aligns it); widgets that cannot bind a field edit slots[i], as target rows do.
struct ConfigModuleStage
{
	double           state[CONFIG_STATE_MAX / sizeof(double)];
	ConfigStageValue slots[CONFIG_STAGE_MAX];
};

// Copies m.stateSize bytes of m.state and reads target rows and offset rows
// whose widget cannot bind their field into their slots.
void StageModule(const ConfigModule& m, ConfigModuleStage* s);

// A header only when a row shows, then one row per shown key, bound into s.
// Offset fields that fit bind directly; other rows bind their staged slots.
// A key shows when it has a label, is not retired or CK_TEXT, has a widget,
// and devBuild holds or neither devOnly nor debugOnlyReader holds.
void AddModuleRows(const ConfigModule& m, ConfigModuleStage* s, bool devBuild, std::vector<SettingsRow>* out);

// Holds each numeric value that differs from its saved one to the loader's
// rule (ConfigClampValue), so the tab, the INI and the next start agree: an
// integer slider's value is rounded and clamped, a float or double clamped.
// A value the loader refuses, or one that is not a finite number, goes back
// to its saved value, as does a target slot that writes the same value as
// it. Clamp lines go to log.
void ClampModuleStage(const ConfigModule& m, ConfigModuleStage* s, const ConfigModuleStage& saved, ConfigLogFn log);

// One INI entry per row whose staged value differs from the saved one,
// offset and target rows alike; returns the count. An entry is appended to a
// file without a line for its key only when its value differs from the
// build's default. Slot-bound offset fields are synchronized before comparison;
// custom rows with choices write their choice's INI text.
int ModuleStageEntries(const ConfigModule& m, const ConfigModuleStage& staged, const ConfigModuleStage& saved,
                       std::vector<IniEntry>* out);
