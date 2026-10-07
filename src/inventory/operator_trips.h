// operator_trips.h - Machine operators keep collecting at each output refill until the main
// inventory and the worn backpack are full, then deliver as vanilla does.
#ifndef KEO_INVENTORY_OPERATOR_TRIPS_H
#define KEO_INVENTORY_OPERATOR_TRIPS_H

namespace keo_inventory {

// Main thread, a startup install step: the row while operatorFillBeforeDeliver is on.
void InstallOperatorTrips(int* installed, int* total);
// Any thread: per-reason counts of the detour's verdicts (OR_COUNT entries).
void OperatorTripsCountersRead(long* out, int n);

} // namespace keo_inventory

#endif
