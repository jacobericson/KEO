// ini_names.h - The files each plugin keeps beside its DLL: its settings, its log, and the
// optimizer's retire record. Every reader takes the name from here.
#ifndef KEO_BASE_INI_NAMES_H
#define KEO_BASE_INI_NAMES_H

const char OPTIMIZER_INI_NAME[]    = "KEO.ini";
const char OPTIMIZER_LOG_NAME[]    = "KEO.log";
const char OPTIMIZER_RETIRE_NAME[] = "KEO.retire.txt";
const char PROFILER_INI_NAME[]     = "KEOProfiler.ini";
const char PROFILER_LOG_NAME[]     = "KEOProfiler.log";

// The previous names, read only by the one-time settings import (legacy_ini_import.h).
const char LEGACY_OPTIMIZER_INI_NAME[]    = "KenshiZoneOpt.ini";
const char LEGACY_OPTIMIZER_RETIRE_NAME[] = "KenshiZoneOpt.retire.txt";
const char LEGACY_PROFILER_INI_NAME[]     = "KenshiZoneProfiler.ini";

#endif
