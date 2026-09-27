// hook_manifest.h — the runtime side of the hook manifest: one id per row of
// plugin/hook_manifest_rows.inc, the install call and the row state.

#ifndef KENSHI_ZONE_OPT_HOOK_MANIFEST_H
#define KENSHI_ZONE_OPT_HOOK_MANIFEST_H

#include "plugin/hook_manifest_policy.h"

enum HookRowId
{
#define HOOK_ROW(id, ...) id,
#include "plugin/hook_manifest_rows.inc"
#undef HOOK_ROW
	HOOK_ROW_COUNT
};

// Installs one row's detour at its RVA. With reverify, the row's prologue is
// checked first through VerifyPrologueByRva, which logs its own lines. On
// success the row is marked installed, *installed (when not NULL) is
// incremented, and the result is NULL. On failure *orig is set NULL and the
// result names the step that refused it (the prologue or the hook). Takes no
// lock, so it is safe on any thread.
const char* HookInstallRow(HookRowId id, void* detour, void** orig, int* installed,
                           bool reverify);

bool HookRowInstalled(HookRowId id);

// The row's want over the config globals as they stand at the call.
bool HookRowWanted(HookRowId id);

HookWantInputs HookWantInputsNow();

// Runs every startup install in its fixed order: the row groups, the module
// installers and the non-hook steps between them. *total is the number of
// counted rows wanted by the config as it stands on entry; *installed counts
// the ones that went in. Main thread, startPlugin, once.
void InstallHooks(int* installed, int* total);

#endif
