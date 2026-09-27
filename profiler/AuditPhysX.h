// PhysX scene-query probe: cursor-ray constants, physics phase enums, per-query samples.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <stddef.h>
#include <stdint.h>

namespace audit
{

// Cursor raycast (verified in IDA 2026-09-15). PlayerInterface::mouseScan
// 0x7FF6B0 -> UtilityT::mouseTraceAll 0x9B2A40 -> (site 0x9B2AE2, probe
// `mouseRay`) UtilityT::traceAll 0x9B28D0:
//   void traceAll(lektor<PhysHitItem>* result, const Vector3* origin,
//                 const Vector3* dir, unsigned group)
// normalises dir, then calls NxScene vt+0x370 raycastAllShapes(ray, report =
// HitCallback{result}, shapesType 3, group, maxDist FLT_MAX, hint 0x11, NULL)
// on GameWorld.physics->nWorld, then qsorts the hits (site 0x9B2A09, probe
// `cursorSort`) with comparator 0x9B0950 (distance at +0x24). HitCallback's
// only vtable slot (0x9BCEC0) appends one 40-byte PhysHitItem per hit: a hand
// (+0x00, 32 bytes), the shape's userData+0x20 collision group (+0x20, -1 when
// the shape has no userData) and the distance (+0x24).
const size_t RVA_TRACE_CALLSEQ   = 0x9B29B1;  // traceAll: scene load .. qsort arguments
const size_t RVA_FLT_MAX_CONST   = 0x169AC38; // `X`, the maxDist traceAll passes (FLT_MAX)
const size_t PHYS_NWORLD         = 0xE8;      // PhysicsInterface::nWorld (NxScene*)
const size_t NXSCENE_RAYCAST_ALL = 0x370;     // NxScene vtable: raycastAllShapes
const size_t NPSCENE_CORE        = 1576;      // NpScene: core Scene*
// The core Scene's query/pruner lock is a mutex holder: Scene+0x298 points to
// a 0x30-byte heap block that begins with the CRITICAL_SECTION (PhysXCore64.dll
// RVAs: the Scene ctor 0x1D4140 allocates and initializes it; the core
// raycastAllShapes 0x2C2180 loads [scene+298h] into rcx for EnterCriticalSection).
// The raycast also sets a held flag at block +0x28 and stores its thread id at
// +0x2C, which nothing clears, so the probe reads the section's own owner.
const size_t CORE_SCENE_LOCK     = 0x298;     // core Scene: mutex holder
const size_t CORE_LOCK_IMPL_OFF  = 0x00;      // holder: mutex implementation pointer
const size_t CORE_LOCK_CS_OFF    = 0x00;      // implementation: RTL_CRITICAL_SECTION
const size_t CORE_LOCK_COUNT     = 0x08;      // RTL_CRITICAL_SECTION::LockCount
const size_t CORE_LOCK_RECURSION = 0x0C;      // RTL_CRITICAL_SECTION::RecursionCount
const size_t CORE_LOCK_OWNER     = 0x10;      // RTL_CRITICAL_SECTION::OwningThread
const unsigned TRACE_HINT_FLAGS  = 0x11;      // NX_RAYCAST_SHAPE | NX_RAYCAST_DISTANCE
const size_t HIT_STRIDE          = 40;        // sizeof(PhysHitItem)
const size_t HIT_GROUP           = 0x20;      // PhysHitItem: int collision group
const size_t HIT_LIST_COUNT      = 0x8;       // lektor<PhysHitItem>: unsigned count
const size_t HIT_LIST_DATA       = 0x10;      //   PhysHitItem* data
const size_t SHAPE_USERDATA      = 0x8;       // NxShape::userData (HitCallback reads [shape+8])
const size_t USERDATA_GROUP      = 0x20;      // userData: int collision group (HitCallback reads [ud+20h])

enum PhysPhase
{
	PP_IDLE, PP_BODY_OTHER, PP_LOCK, PP_PRE_OTHER, PP_MAKE, PP_GROUP, PP_IMPULSE,
	PP_HULL_DESTROY, PP_ACTOR_DESTROY, PP_TERRAIN, PP_HULL_APPLY, PP_SIMULATE,
	PP_FLUSH, PP_FETCH, PP_CONTROLLER, PP_POST, PP_COUNT
};
extern const char* PHYS_PHASE_NAMES[PP_COUNT];

enum PhysOp
{
	PO_MAKE, PO_GROUP, PO_IMPULSE, PO_HULL_DESTROY, PO_ACTOR_DESTROY, PO_TERRAIN,
	PO_HULL_APPLY, PO_COUNT
};

enum PhysQueryKind { PQ_MOUSE_ALL, PQ_INDOORS, PQ_KIND_COUNT };
extern const char* PHYS_QUERY_NAMES[PQ_KIND_COUNT];

enum PhysOwner { POW_UNKNOWN, POW_NONE, POW_MAIN, POW_PHYS_BACK, POW_AI, POW_OTHER, POW_COUNT };
extern const char* PHYS_OWNER_NAMES[POW_COUNT];

struct PhysRunSample
{
	bool      valid;
	int       runSeq;
	float     runMs, lockMs, preMs;
	float     phaseMs[PP_COUNT];
	int       hulls;
	int       queued[PO_COUNT];
	int       calls[PO_COUNT];
};

struct PhysQuerySample
{
	double    t;
	float     ms;
	int       ordinal;
	int       kind;
	int       physRunning;
	int       runSeq;
	int       phase;
	int       owner;
	unsigned  ownerTid;
	int       lockValid;
	int       lockCount;
	int       recursion;
};

const int MAX_PHYS_QUERIES = 8;

uintptr_t CurrentNpScene();
bool PhysCoreLockSample(uintptr_t npScene, int* lockCount, int* recursion, unsigned* ownerTid);
int PhysOwnerClass(unsigned tid);
void PhysQueryEnter(int kind, uintptr_t npScene);
void PhysQueryExit(int kind, LONGLONG ticks);

} // namespace audit
