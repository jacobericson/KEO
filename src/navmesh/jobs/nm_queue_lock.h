// nm_queue_lock.h - The generator's queue lock (+152): one scope type, and the reference the functions needing it take.
// The NavMesh threads and the main thread. Only the drain observer takes a lock under it (the done mutex +200);
// nothing else is taken while it is held, and nothing here allocates or logs.
#ifndef KEO_NM_QUEUE_LOCK_H
#define KEO_NM_QUEUE_LOCK_H

#include "game/game.h"

// The queue mutex taken and released the way the game's addJob takes it. The
// busy bridge's callbacks call these two; every other holder is an NmQueueLock.
static inline void NmQueueMutexLock(uintptr_t nmg)
{
	char initBuf[16];
	void* initResult = game::g_gameFn.fn_pathBuilderInit(initBuf);
	game::g_gameFn.fn_pathBuilderFinalize((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)), initResult);
}

static inline void NmQueueMutexUnlock(uintptr_t nmg)
{
	game::g_gameFn.fn_readerUnlock((void*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_queue_mutex, 152)));
}

// A hold of the queue lock. A navmesh function whose name ends in Locked takes
// a reference to one as its first parameter: the caller states it holds +152
// for the call. Release() is for a region whose normal paths let go before
// the scope ends, at the statement that released it; the destructor releases
// only on a C++ unwind. Acquire() takes a hold after the scope exists: the bg
// dispatch keeps its scope in hook_dispatchJob's frame, and its pick
// re-acquires after a wait.
class NmQueueLock
{
public:
	NmQueueLock() : m_nmg(0) {}
	explicit NmQueueLock(uintptr_t nmg) : m_nmg(0) { Acquire(nmg); }
	~NmQueueLock() { Release(); }

	void Acquire(uintptr_t nmg)
	{
		NmQueueMutexLock(nmg);
		m_nmg = nmg;
	}
	void Release()
	{
		if (!m_nmg)
			return;
		uintptr_t nmg = m_nmg;
		m_nmg = 0;
		NmQueueMutexUnlock(nmg);
	}
	uintptr_t Nmg() const { return m_nmg; }

private:
	uintptr_t m_nmg;
	NmQueueLock(const NmQueueLock&);
	NmQueueLock& operator=(const NmQueueLock&);
};

#endif
