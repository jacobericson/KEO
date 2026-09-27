// The config key table, DEV (built with /DZONEOPT_DEBUG /DZONEHAND_STEP=3): see config_table_checks.h.

#include "config_table_checks.h"

// The bench units the custom rows call need these from the runtime; loading
// the config never reaches them.
bool ApplyRenderConfig(const RenderConfig&) { return true; }
bool BenchWindowInForeground() { return false; }
int BenchLoadedZoneCount() { return 0; }

int main()
{
	return RunConfigTableChecks("config_table_dev_units");
}
