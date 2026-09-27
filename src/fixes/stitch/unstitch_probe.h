#ifndef KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_H
#define KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_H

// Read-only diagnostic detour on NavMesh::deleteInstance, DEV builds only,
// off unless unstitchProbe is set.
//
// Teardown of a navmesh instance ends in a cross-section un-stitch that walks
// the dying graph instance's streaming sets and, for every graph connection in
// a set naming this instance's section, indexes the *opposite* instance's node
// map with the index recorded in the connection. That read is guarded only by
// "is the map non-empty"; the index itself is never compared with the map's
// size. A wild value there faults on the load that follows.
//
// This detour walks the same records, in the same order, before handing the
// call to the original, and reports every index that would land outside the
// map it is about to be used on. It answers one question and adds nothing
// else: was such an index out of bounds -- a record left over from an earlier
// generation of the opposite section -- or in bounds and holding a value that
// the opposite map itself no longer describes.
//
// It takes no lock, allocates nothing, writes nothing, and always calls the
// original whatever it saw. Every game read is fault-guarded; a record it
// cannot read is skipped and counted.
void InstallUnstitchProbe(int* installed, int*);

#endif // KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_H
