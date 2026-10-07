// job_counters.h - DEV diagnostics for player job trips: the haul-size counter on
// Task_EmptyMachine's amount slot and the Jobs: heartbeat. Empty in a PROD build.
#ifndef KEO_INVENTORY_JOB_COUNTERS_H
#define KEO_INVENTORY_JOB_COUNTERS_H

namespace keo_inventory {

// Main thread, a startup install step (DEV): the haul counter's row.
void InstallJobCounters(int* installed, int* total);
// Main thread, every frame from the camera tick: the Jobs: line every 60 s when anything moved.
void OperatorTripsTick(double now, bool saveLoading);

} // namespace keo_inventory

#endif
