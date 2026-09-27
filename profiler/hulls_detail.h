// hulls_detail.h - Hull diagnostic records, shared state and detour tables.
// Included only by AuditHulls.cpp and hulls_*.cpp. Detours run on any
// thread that destroys a hull, the handler on the faulting thread, and the
// printers on the main thread (Hulls_OnFrameStarted). Detours and the
// handler neither allocate nor take locks.

#ifndef KENSHI_HULLS_DETAIL_H
#define KENSHI_HULLS_DETAIL_H

#include "KenshiFrameAudit_internal.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreorder-ctor"
#endif
#include "AuditHullsTable.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace audit {
namespace audithulls_detail {

using namespace hulls;

// MessageChain: vtable, main-thread lektor at +0x08, back-thread lektor at
// +0x20. lektor: vtable, count +0x08, capacity +0x0C, data +0x10.
const size_t PI_MAKE_MAIN_COUNT    = 0x1B0;   // hullsToMake (+0x1A0)
const size_t PI_MAKE_MAIN_DATA     = 0x1B8;
const size_t PI_DESTROY_MAIN_COUNT = 0x200;   // hullsToDestroy (+0x1F0)
const size_t PI_DESTROY_MAIN_DATA  = 0x208;
const size_t PI_DESTROY_BACK_COUNT = 0x218;
const size_t PI_DESTROY_BACK_DATA  = 0x220;
KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToMake_mainThreadData_count, PI_MAKE_MAIN_COUNT);
KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToDestroy_mainThreadData_count, PI_DESTROY_MAIN_COUNT);
KLIB_ASSERT_OFFSET(PhysicsInterface_hullsToDestroy, PI_DESTROY_MAIN_COUNT - 0x10);

// SimplePhysXEntity family: Ogre::MovableObject*, which slot 1 hands to
// GameWorld::destroy and nulls before it queues the entity.
const size_t ENTITY_MOVABLE = 0x20;

// threadJunkPreBT's delete loop (`call [rax]` at 0x4CBFA6) returns here.
const size_t RVA_CONSUMER_RET = 0x4CBFA8;

const size_t RVA_JUNK_PRE_BT = 0x4CBB90;   // PhysicsActual::threadJunkPreBT(this)
const size_t RVA_ZONE_RELEASE = 0x399710;  // releases a zone's shared hulls (ZoneMap::_dactivateMT, unloadAllZones)

enum EventKind
{
	EV_PUSH_HULL, EV_PUSH_ENTITY, EV_PUSH_SCYTHE, EV_PUSH_ROOT, EV_PUSH_BASE,
	EV_PUSH_UNLOAD, EV_PUSH_INLINE,
	EV_DTOR_HULL, EV_DTOR_SIMPLE, EV_DTOR_BOX, EV_DTOR_CAPSULE, EV_DTOR_DOOR,
	EV_DTOR_SCYTHE, EV_DTOR_ROOT, EV_DTOR_RAGDOLL,
	EV_FLUSH, EV_BATCH, EV_BATCH_ENTRY, EV_MAKE,
	EV_COUNT
};
const int VPUSH_KINDS = EV_PUSH_BASE + 1;
const int PUSH_KINDS  = EV_PUSH_INLINE + 1;
const int DTOR_KINDS  = EV_DTOR_RAGDOLL - EV_DTOR_HULL + 1;

enum RecFlags
{
	F_OFFMAIN  = 1,   // not the main thread
	F_MOVABLE  = 2,   // entity destructor with its MovableObject still attached
	F_DELETE   = 4,   // destructor frees the block
	F_CONSUMER = 8    // destructor called from threadJunkPreBT's loop
};

struct HullRec
{
	volatile LONG64 seq;   // ring index + 1, written last; 0 while being written
	LONG64          qpc;
	uintptr_t       ptr;   // the hull; EV_FLUSH / EV_BATCH: entry count
	uintptr_t       ret;   // caller's return address, 0 when unknown
	DWORD           tid;
	unsigned short  kind;
	unsigned short  flags;
	unsigned        vtRva; // the object's vtable as an exe RVA, 0 when unknown
	unsigned        pad;
};

const LONG64 RING = 4096;     // power of two
const int    SNAP = 64;
const int    ANOM_KEEP = 8;
const unsigned TABLE_SLOTS = 1u << 18;

struct AnomRec
{
	volatile LONG ready;
	HullRec       ev;
	LONG64        prev, prevPushQpc, prevDtorQpc;
};

// Game-side zone unloads, main thread only.
struct UnloadStamp { LONGLONG qpc; int x, y; };
const int UNLOADS = 32;

uintptr_t SafeField(uintptr_t obj, size_t off);

// Entry i of a game list another thread may grow or reallocate meanwhile.
inline uintptr_t ListEntry(const uintptr_t* data, unsigned i)
{
	return SafeField((uintptr_t)data, (size_t)i * sizeof(uintptr_t));
}

typedef void* (*Push_t)(void*);
typedef void* (*Dtor_t)(void*, unsigned);
typedef void  (*Junk_t)(void*);
typedef void  (*Release_t)(void*);

extern const char* EVENT_NAMES[EV_COUNT];
extern const char* STATE_NAMES[ST_COUNT];
extern const char* ANOMALY_NAMES[AN_COUNT];

extern volatile LONG g_on;
extern Table g_table;
extern uintptr_t g_exeBase, g_exeEnd, g_gameWorld, g_consumerRet;
extern volatile DWORD g_physTid;
extern HullRec g_ring[RING];
extern volatile LONG64 g_ringHead;
extern HullRec g_snap[AN_COUNT][SNAP];
extern volatile LONG g_snapState[AN_COUNT];
extern LONG64 g_snapEnd[AN_COUNT];
extern AnomRec g_anom[AN_COUNT][ANOM_KEEP];
extern volatile LONG g_anomCount[AN_COUNT];
extern int g_anomPrinted[AN_COUNT];
extern volatile LONG g_push[PUSH_KINDS];
extern volatile LONG g_dtor[DTOR_KINDS];
extern volatile LONG g_dtorConsumer, g_made, g_flushes, g_batches;
extern LONG g_batchMax;
extern volatile LONG64 g_batchEntries;
extern volatile LONG g_inJunk;
extern const uintptr_t* volatile g_junkData;
extern volatile unsigned g_junkCount;
extern volatile uintptr_t g_lastConsumed;
extern HANDLE g_crashFile;
extern volatile LONG g_crashWritten;
extern PVOID g_veh;
extern __declspec(thread) int t_auditGuard;
extern UnloadStamp g_unloads[UNLOADS];
extern int g_unloadCount;
extern LONGLONG g_lastReport;

extern Push_t oPush[VPUSH_KINDS];
extern Dtor_t oDtor[DTOR_KINDS];
extern Junk_t oJunk;
extern Release_t oRelease;
extern const Push_t PUSH_DETOURS[VPUSH_KINDS];
extern const Dtor_t DTOR_DETOURS[DTOR_KINDS];

unsigned VtRva(uintptr_t obj);
uintptr_t Physics();
HullRec Ev(int kind, uintptr_t ptr, uintptr_t ret, DWORD tid, unsigned flags, unsigned vtRva);
void Emit(HullRec& r);
bool CopyRec(LONG64 i, HullRec* out);
void Flag(int an, const HullRec& ev, const Result& r);
void OnPush(int kind, uintptr_t obj, uintptr_t ret);
void OnDtor(int kind, uintptr_t obj, uintptr_t ret, unsigned deleteFlags);
void ReleaseDetour(void* owner);
void JunkDetour(void* self);
LONG WINAPI HullFaultHandler(PEXCEPTION_POINTERS x);
void PrintAnomalies();
void PrintSnapshot(int an);
void PrintStats();


} // namespace
using namespace audithulls_detail;

} // namespace audit

#endif
