// nm_adjacency_internal.h - Private adjacency state and helpers shared by the nm_adjacency*.cpp units.
#ifndef KENSHI_ZONE_OPT_NM_ADJACENCY_INTERNAL_H
#define KENSHI_ZONE_OPT_NM_ADJACENCY_INTERNAL_H
#include "navmesh/scheduling/nm_adjacency.h"
#include "navmesh/scheduling/nm_adjacency_counters.h"
#include "navmesh/nm_workers.h"

namespace nm_adjacency_detail {
enum { MODE_OFF = 0, MODE_ENFORCE, MODE_COUNT, MODE_COUNT_NO_OBSERVER };
extern volatile LONG s_checkerPresent;
extern const size_t OFF_NMG_THREAD_RUNNING;
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
extern const LONGLONG kAgeAfterMs;
extern const DWORD kBgStopPollMs;
extern const LONGLONG kBgSliceMs;
extern const LONG kMaxViolLines;
extern double s_nextBeat;
extern const double kBeatSeconds;
LONG Read(volatile LONG* p);
LONG64 Read64(volatile LONG64* p);
void NoteMax64(volatile LONG64* slot, LONG64 v);
void NoteMax(volatile LONG* slot, LONG v);
void Wake();
void LockQueue(uintptr_t nmg);
void UnlockQueue(uintptr_t nmg);
bool ReadU32Guarded(const void* at, unsigned int* out);
// Writers hold +152, so there is one at a time; the sequence lets the checker
// read without the lock.
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
