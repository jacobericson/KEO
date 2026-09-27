#ifndef KENSHI_ZONE_OPT_FIXES_UNSTITCH_GUARD_H
#define KENSHI_ZONE_OPT_FIXES_UNSTITCH_GUARD_H

// Detour on the cross-section un-stitch a navmesh instance's teardown runs.
//
// The native walk reads the opposite instance's node map at each recorded
// connection's opposite node index and bounds that index against nothing. A
// record whose index is past the end of the map reads adjacent heap and feeds
// the result straight into a second indexed load, which faults. That is a
// measured condition, not a hypothetical: a record carrying index 58 against a
// 55-entry map, twice at the same site.
//
// The detour scans the records first. When every recorded index lands inside
// the map it calls the original unchanged, so a healthy teardown is untouched.
// When one does not, it performs the same walk itself and skips that record
// alone -- which is what the walk would have done for it anyway, because an
// index past the end names a node the opposite instance does not have, so
// there is no edge there to remove.
//
// Runs on the section-manager thread, inside the changeMutex the teardown's
// caller holds.
void InstallUnstitchGuard(int* installed, int*);

// The detour is live this session.
bool UnstitchGuardInstalled();

#endif // KENSHI_ZONE_OPT_FIXES_UNSTITCH_GUARD_H
