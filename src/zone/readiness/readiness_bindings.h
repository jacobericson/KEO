// readiness_bindings.h - Zone readiness layout and shared-lock query bindings.
// Included through game.h.

#ifndef KENSHI_ZONE_OPT_READINESS_BINDINGS_H
#define KENSHI_ZONE_OPT_READINESS_BINDINGS_H

#include "game/klib_members.h"
#include "base/core.h"

// ---- isContentPending readiness classification ----
//
// Readiness classification in hook_isContentPending (readiness_hook.cpp). Verified in
// the Steam 1.0.65 IDB. The SectionManager (pauseState.navmesh,
// the `manager` argument of isContentPending) holds:
//   +0x088  hkaiWorld* of the main navmesh world. Its +0x20 is the
//           hkaiStreamingCollection: instances data +0x20 (48-byte entries,
//           the first qword is the hkaiNavMeshInstance*), size +0x28. Same
//           reads as NavMesh__snapToFace 0x3A1F70.
//   +0x1E0  boost::shared_mutex over the section map. Taken exclusively (timed
//           lock) by isContentPending 0x3AB4F0, getOrCreateSection 0x3AB1F0,
//           the erase in 0x3AB450 and processZoneWorkItem 0x3ADF30.
//   +0x200  boost::shared_mutex over the world. Section adds (contentStream
//           0x3AE66B) and world removals (removeNavInstance 0x3AB8A0) take it
//           exclusively; the query functions (snapToFace 0x3A1F70 and 16 more)
//           try-lock it shared and give up when that fails.
//   +0x220  std::map<zone key, SectionState*> (key y + 10000*x); its header
//           node pointer is at +0x228; the value is at node +0x20.
// hkaiNavMeshInstance +0x1A0 is its section uid; an outdoor zone's is
// gridX | (gridY << 8) (the section's +60, copied by 0xD09EB0 through
// 0x3ACD20). +0x1A4 is its slot in the collection, -1 when not in the world
// (addInstance 0xD0D8F0 writes it, removeInstance 0xD0DC30 resets it).
// hkaiStreamingCollection's own uid lookup is 0xD0CD40 (a linear scan).
const size_t OFF_RDY_SM_WORLD            = 0x88;
KLIB_ASSERT_OFFSET(NavMesh_world, OFF_RDY_SM_WORLD);
const size_t OFF_RDY_SM_MAP_LOCK         = 0x1E0;
KLIB_ASSERT_OFFSET(NavMesh_mutex, OFF_RDY_SM_MAP_LOCK);
const size_t OFF_RDY_SM_WORLD_LOCK       = 0x200;
KLIB_ASSERT_OFFSET(NavMesh_changeMutex, OFF_RDY_SM_WORLD_LOCK);
const size_t OFF_RDY_SM_SECTION_MAP      = 0x220;
KLIB_ASSERT_OFFSET(NavMesh_sectors, OFF_RDY_SM_SECTION_MAP);
const size_t OFF_RDY_SM_SECTION_MAP_HEAD = 0x228;
KLIB_ASSERT_OFFSET(NavMesh_sectors__Myhead, OFF_RDY_SM_SECTION_MAP_HEAD);
const size_t OFF_RDY_SM_PENDING_SECTIONS = 0x278;  // 632: sections not yet added
KLIB_ASSERT_OFFSET(NavMesh_addList_count, OFF_RDY_SM_PENDING_SECTIONS);
const size_t OFF_RDY_MAPNODE_VALUE       = 0x20;
static_assert(OFF_RDY_MAPNODE_VALUE == KLIB_OFF_SectorMapNode_value + KLIB_OFF_SectorValue_second, "OFF_RDY_MAPNODE_VALUE composed legacy offset drift");
const size_t OFF_RDY_WORLD_COLLECTION    = 0x20;
const size_t OFF_RDY_SC_DATA             = 0x20;
const size_t OFF_RDY_SC_COUNT            = 0x28;
const size_t RDY_SC_ENTRY_SIZE           = 48;
const size_t OFF_RDY_NMI_SECTION_UID     = 0x1A0;
const size_t OFF_RDY_NMI_RUNTIME_INDEX   = 0x1A4;

// Called, never hooked (no build-gate rows needed).
//   SectionManager__lookupSection 0x3BBA60: std::map lower_bound on the zone
//     key; writes the header node to *out when there is no entry. Pure reads.
//   boost::shared_mutex::unlock 0x25C3D0 and unlock_shared 0x168E10: the
//     game's own release paths (CAS + ReleaseSemaphore, no allocation).
const size_t RVA_LOOKUP_SECTION       = 0x3BBA60;
const size_t RVA_BOOST_UNLOCK         = 0x25C3D0;
const size_t RVA_BOOST_UNLOCK_SHARED  = 0x168E10;

typedef void* (*lookupSection_t)(void* sectionMap, void** outNode, const int* zonePos);
typedef unsigned int (*boostUnlock_t)(void* mutex);
typedef long (*boostUnlockShared_t)(void* mutex);

extern lookupSection_t      fn_lookupSection;
extern boostUnlock_t        fn_boostUnlock;
extern boostUnlockShared_t  fn_boostUnlockShared;

// try_lock_shared on a boost::shared_mutex state word (impl in readiness_hook.cpp,
// where the bit layout is documented). Release with fn_boostUnlockShared.
bool BoostTryLockShared(volatile LONG* state);
// ---- end isContentPending readiness classification ----

#endif // KENSHI_ZONE_OPT_READINESS_BINDINGS_H
