// nm_adjacency_internal.h - Private adjacency state and helpers shared by the nm_adjacency*.cpp units.
#ifndef KENSHI_ZONE_OPT_NM_ADJACENCY_INTERNAL_H
#define KENSHI_ZONE_OPT_NM_ADJACENCY_INTERNAL_H
#include "navmesh/scheduling/nm_adjacency.h"
#include "navmesh/scheduling/nm_adjacency_counters.h"
#include "navmesh/nm_workers.h"

namespace nm_adjacency_detail {
const LONG      kMaxViolLines  = 8;

enum { MODE_OFF = 0, MODE_ENFORCE, MODE_COUNT, MODE_COUNT_NO_OBSERVER };
// Threads: InstallNavMeshAdjacency writes s_mode, s_observerInstalled,
// s_observerWhy, g_adjEvent and s_qpf once at hook install on the main thread;
// every thread only reads them after that. The stitch-source install sets
// s_checkerPresent once. The NavMesh workers, the bg thread and the path
// thread's drain observer change g_reg, g_regSeq and g_pubHint under the
// generator queue lock +152, except the bg thread's own look stamps
// (bgLastLook, bgLooked), which only it writes, unlocked; the stitch checker
// copies g_reg without the lock, sequence-checked. InstallNavMeshAdjacency
// also initialises g_reg and sets s_nextBeat to 0 at the same install. t_own
// and t_ownTask are the claiming thread's own claim; s_nextBeat is the
// main-thread heartbeat's.
extern volatile LONG s_checkerPresent;
extern int s_mode;
extern bool s_observerInstalled;
extern const char* s_observerWhy;
extern NmAdjRegistry g_reg;
extern volatile LONG g_regSeq;
extern volatile LONG g_pubHint;
extern HANDLE g_adjEvent;
extern __declspec(thread) int t_own;
extern __declspec(thread) unsigned __int64 t_ownTask;
extern LONGLONG s_qpf;
extern double s_nextBeat;
LONG Read(volatile LONG* p);
LONG64 Read64(volatile LONG64* p);
void NoteMax64(volatile LONG64* slot, LONG64 v);
void NoteMax(volatile LONG* slot, LONG v);
void Wake();
void LockQueue(uintptr_t nmg);
void UnlockQueue(uintptr_t nmg);
bool ReadU32Guarded(const void* at, unsigned int* out);
// Entry writers hold +152, so there is one at a time; the sequence lets the
// diagnostic checker copy e/high without it. Bg-only time fields have their
// own unlocked writer and are not part of that copied snapshot.
static inline void WBegin() { InterlockedIncrement(&g_regSeq); }
static inline void WEnd()
{
	InterlockedExchange(&g_pubHint, g_reg.published);
	NoteMax(&s_liveMax, g_reg.live);
	NoteMax(&s_pubMax, g_reg.published);
	InterlockedIncrement(&g_regSeq);
}
typedef void (*nmgUpdate_t)(void* nmg);
extern nmgUpdate_t orig_nmgUpdate;
void hook_nmgUpdate(void* nmg);
} // namespace nm_adjacency_detail
#endif // KENSHI_ZONE_OPT_NM_ADJACENCY_INTERNAL_H
