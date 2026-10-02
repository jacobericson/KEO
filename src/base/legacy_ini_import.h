// legacy_ini_import.h - The one-time import of a settings file left beside the DLL under the
// plugin's previous name: a pure decision and the file steps around it. The old file is renamed
// to its marker (<old>.imported) before anything is written, so an import happens at most once
// per folder; a replaced new file is kept as <new>.replaced.
// To remove the import: delete this file and legacy_ini_import.cpp, their coresrc.txt,
// profsrc.txt and suites.txt rows, the call in LoadConfig and the profiler's call and adapter,
// the previous names in ini_names.h, and the two shared-source paths in check_coresrc.py with
// every test row that names them.
#ifndef KEO_BASE_LEGACY_INI_IMPORT_H
#define KEO_BASE_LEGACY_INI_IMPORT_H

#include <string>

enum LegacyIniOutcome
{
	LEGACY_INI_NONE = 0,            // no old file: nothing happens, nothing is logged
	LEGACY_INI_LEFT_ALONE_DONE,     // the marker exists: an import already ran in this folder
	LEGACY_INI_RENAME_ONLY,         // the old file carries nothing the new file lacks
	LEGACY_INI_LEFT_ALONE_NEW_SET,  // the new file already has a live line (optimizer only)
	LEGACY_INI_IMPORT,              // the old bytes replace the new file
	LEGACY_INI_FAILED               // the action's step failed (never a decision)
};

struct LegacyIniInputs
{
	bool        oldExists;
	std::string oldText;
	bool        markerExists;
	bool        newExists;
	std::string newText;
	bool        refuseWhenNewHasLiveLine;
};

// Decided in this order: NONE, LEFT_ALONE_DONE, RENAME_ONLY (no live line in the old file, or the
// same live lines as the new file, by SplitIniLine, in order), LEFT_ALONE_NEW_SET, IMPORT.
LegacyIniOutcome LegacyIniDecide(const LegacyIniInputs& in);

typedef void (*LegacyIniLogFn)(const std::string& line);

// Main thread, startup, before the settings file is read; file calls only, and takes no lock
// itself (the log callback may take the plugin's log lock). Deletes
// dir + staleName first when staleName is set. Logs at most one line through log.
LegacyIniOutcome LegacyIniImport(const std::string& dir, const char* oldName, const char* newName,
                                 const char* staleName, bool refuseWhenNewHasLiveLine, LegacyIniLogFn log);

#endif
