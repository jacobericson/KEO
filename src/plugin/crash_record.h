#ifndef KENSHI_ZONE_OPT_PLUGIN_CRASH_RECORD_H
#define KENSHI_ZONE_OPT_PLUGIN_CRASH_RECORD_H

// The crash recorder's entry points and state, as startPlugin uses them: it
// fills the two paths and the module snapshot and registers both handlers.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "diag/module_bases.h"
#include "fixes/crash_claim.h"

extern char g_crashFilePath[MAX_PATH];
extern char g_cppExFilePath[MAX_PATH];

extern ModuleBaseEntry g_moduleBases[kMaxModuleBases];
extern int             g_moduleBaseCount;

extern LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter;

LONG WINAPI NavMeshCrashHandler(PEXCEPTION_POINTERS pExInfo);
LONG WINAPI ZoneOptUnhandledFilter(PEXCEPTION_POINTERS pExInfo);

void SnapshotModuleBases();

CrashClaimOutcome ClaimPreviousCrashDump(const std::string& dllDir, unsigned long* outLastError);

#endif
