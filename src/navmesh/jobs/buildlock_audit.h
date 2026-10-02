#ifndef KEO_BUILDLOCK_AUDIT_H
#define KEO_BUILDLOCK_AUDIT_H

// Offsets the audit reads: instance -> mesh and uid; mesh -> streaming-set
// array (data, count); entry stride and the entry's "other uid" field.
struct BuildLockLayout { int instMesh, instUid, meshSets, meshSetCount, setStride, setOtherUid; };

// Streaming-set links are written in pairs: J's mesh lists I exactly when I's
// mesh lists J. Counts the instances in `list` for which that fails. Skips NULL
// entries, J itself and instances without a mesh.
int StreamingPairMismatches(const void* inst, const void* const* list, int count, const BuildLockLayout& lay);

#endif
