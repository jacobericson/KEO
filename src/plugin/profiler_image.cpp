// profiler_image.cpp - The profiler image lookup's frame tick and its one report line.
// Main thread only.

#include "plugin/profiler_image.h"
#include "base/core.h"
#include "plugin/crash_record.h"
#include <stdio.h>

void ProfilerImageTickMain(double now)
{
	if (!ProfilerImageResolveTick(now))
		return;
	char line[128];
	double foundSec = ProfilerImageFoundSec();
	if (foundSec >= 0.0)
		_snprintf_s(line, sizeof(line), _TRUNCATE, "ProfilerImage: found after %.1f s, crashSkipProfiler=%ld",
		            foundSec, CrashSkipProfilerCount());
	else
		_snprintf_s(line, sizeof(line), _TRUNCATE, "ProfilerImage: absent after 60 s, crashSkipProfiler=%ld",
		            CrashSkipProfilerCount());
	LogMsg(line);
}
