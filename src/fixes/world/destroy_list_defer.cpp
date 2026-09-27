// destroy_list_defer.cpp - Destroy-list probing and deferred inserts.
// Main thread for initialization, probe, stats, flush and drop; any thread
// for the insert detour. deferCS is a leaf and never spans the original insert.
// Probe logging takes logCS without deferCS held.

#include "base/core.h"
#include "fixes/world/destroy_list_defer.h"
#include "game/klib_members.h"

// _ReturnAddress: the destroyListOE inserter hook records who called it from
// off the main thread, and the call site is the whole point of the record.
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)


// =========================================================================
// destroyListOE invariant probe; contract in destroy_list_defer.h.
// =========================================================================

// Guards the destroy-list queue; initialised at the earliest main-thread point.
static CRITICAL_SECTION deferCS;
static bool deferCSInitialized = false;

void DestroyListDeferInit()
{
	if (!deferCSInitialized)
	{
		// Before any hook is installed, so no off-main thread can reach the
		// queue while the lock is still uninitialised.
		InitializeCriticalSection(&deferCS);
		deferCSInitialized = true;
	}
}

// Offsets inside the GameWorld singleton (the IDB's `pauseState`, RVA
// 0x21330B0). Both containers are Ogre `boost::unordered` ptr-node sets with
// the same three-field shape, read straight off sub_14079CD00's drain loops:
//
//   destroyListOE   base +0x680: bucket_count +0x698, size +0x6A0, buckets +0x6B8
//                   (drain at 0x79CEA5, the loop that faulted at +0x1C1)
//   killListPhase0  base +0x7D0: bucket_count +0x7E8, size +0x7F0, buckets +0x808
//                   (drain at the top of the same function, lines 121-143)
//
// The sentinel node list hangs off buckets[bucket_count] in both, which is the
// pointer the drain dereferences without a null check.
static const size_t OFF_GW_DESTROYOE_NBUCKETS = 0x698;
static_assert(OFF_GW_DESTROYOE_NBUCKETS == KLIB_OFF_GameWorld_destroyListOE + KLIB_OFF_MovableSetTable_bucket_count_, "OFF_GW_DESTROYOE_NBUCKETS composed legacy offset drift");
static const size_t OFF_GW_DESTROYOE_SIZE     = 0x6A0;
static_assert(OFF_GW_DESTROYOE_SIZE == KLIB_OFF_GameWorld_destroyListOE + KLIB_OFF_MovableSetTable_size_, "OFF_GW_DESTROYOE_SIZE composed legacy offset drift");
static const size_t OFF_GW_DESTROYOE_BUCKETS  = 0x6B8;
static_assert(OFF_GW_DESTROYOE_BUCKETS == KLIB_OFF_GameWorld_destroyListOE + KLIB_OFF_MovableSetTable_buckets_, "OFF_GW_DESTROYOE_BUCKETS composed legacy offset drift");
static const size_t OFF_GW_KILL0_NBUCKETS     = 0x7E8;
static_assert(OFF_GW_KILL0_NBUCKETS == KLIB_OFF_GameWorld_killListPhase0 + KLIB_OFF_RootSetTable_bucket_count_, "OFF_GW_KILL0_NBUCKETS composed legacy offset drift");
static const size_t OFF_GW_KILL0_SIZE         = 0x7F0;
static_assert(OFF_GW_KILL0_SIZE == KLIB_OFF_GameWorld_killListPhase0 + KLIB_OFF_RootSetTable_size_, "OFF_GW_KILL0_SIZE composed legacy offset drift");
static const size_t OFF_GW_KILL0_BUCKETS      = 0x808;
static_assert(OFF_GW_KILL0_BUCKETS == KLIB_OFF_GameWorld_killListPhase0 + KLIB_OFF_RootSetTable_buckets_, "OFF_GW_KILL0_BUCKETS composed legacy offset drift");

// A bucket count past this is garbage, not a container we should index into.
static const unsigned __int64 MAX_SANE_BUCKETS = 0x100000;

static uintptr_t s_gameWorld = 0;

// Inserter instrumentation. Plain volatile LONGs touched only by Interlocked*,
// so the hook needs no lock and no CRT.
static volatile LONG s_lastInsertTid = 0;
static volatile LONG s_insMain       = 0;
static volatile LONG s_insOther      = 0;
static volatile LONG s_ringNext      = 0;
static const int     RING_SIZE       = 8;
static volatile uintptr_t s_ring[RING_SIZE] = { 0 };
static bool          s_ringPrinted   = false;

// Deferred-insert queue (mitigation, destroy_list_defer.h). Fixed capacity, no allocation:
// the hook runs inside an engine destructor on a background thread, where a
// heap call is a hazard of its own. Both fields are written only under deferCS.
// The gameWorld pointer is carried per entry rather than assumed constant, so
// the flush replays exactly the call the game made.
struct DeferredInsert
{
	void* gameWorld;
	void* movable;
};
static const int    DEFER_CAP   = 1024;
static DeferredInsert s_defer[DEFER_CAP];
static int          s_deferCount = 0;
static bool         s_deferEnabled = false;

static volatile LONG s_deferred   = 0;
static volatile LONG s_flushed    = 0;
static volatile LONG s_deferFull  = 0;
static volatile LONG s_dedup      = 0;
static volatile LONG s_dropClear  = 0;
static volatile LONG s_deferEnt   = 0;
static volatile LONG s_deferNonEnt = 0;

// Bound on one frame's flush: 8 batches of FLUSH_BATCH. A queue deeper than
// that is drained over the following frames rather than in one hitch, which
// also keeps the worst case of this loop independent of DEFER_CAP.
static const int FLUSH_BATCH    = 32;
static const int FLUSH_BATCHES  = 8;

// Removes every queued entry naming `movable`. deferCS must be held. Returns
// how many were removed. Order in the queue is irrelevant (the destination is
// a hash set), so removal swaps the last entry down instead of shifting.
static int RemoveQueuedLocked(void* movable)
{
	int removed = 0;
	for (int i = 0; i < s_deferCount; )
	{
		if (s_defer[i].movable == movable)
		{
			s_defer[i] = s_defer[--s_deferCount];
			removed++;
		}
		else
		{
			++i;
		}
	}
	return removed;
}

// True if the object's Ogre movable type is "Entity", i.e. the branch of
// sub_140799BE0 that also de-registers the object from the loader's queued and
// preloaded lists (sub_140449240). Safe on any thread: vtable slot 4
// (`getMovableType`) is a const getter that returns a reference to its class's
// static type-name string, and the game itself makes exactly this virtual call
// on exactly this object on this thread at this instant — the deferred path is
// not taking a risk the unmodified binary does not already take. No allocation
// and no CRT beyond memcmp.
static bool MovableTypeIsEntity(void* movable)
{
	if (!movable)
		return false;
	typedef const void* (__fastcall *getMovableType_t)(void*);
	const void* const* vt = *(const void* const* const*)movable;
	if (!vt)
		return false;
	getMovableType_t getType = (getMovableType_t)vt[4];   // +32
	if (!getType)
		return false;

	// MSVC 2010 std::string: [0]=buffer or heap pointer, [2]=size, [3]=capacity;
	// the buffer is out of line once capacity >= 16.
	const size_t* s = (const size_t*)getType(movable);
	if (!s)
		return false;
	size_t len = *(const size_t*)KLIB_MEMBER(5, s, StdString_size, 16);
	const char* buf = (*(const size_t*)KLIB_MEMBER(5, s, StdString_capacity, 24) >= 16) ? *(const char* const*)KLIB_MEMBER(5, s, StdString_pointer, 0) : (const char*)KLIB_MEMBER(5, s, StdString_buffer, 0);
	// Ogre::EntityFactory::FACTORY_TYPE_NAME. The game compares against the
	// imported global (IAT slot RVA 0x2247F08); comparing against its value
	// avoids an import read for a counter.
	return (len == 6 && buf && memcmp(buf, "Entity", 6) == 0);
}

destroyListInsert_t orig_destroyListInsert = NULL;

void SetDestroyListBase(uintptr_t gameWorld)
{
	s_gameWorld = gameWorld;
}

// Standalone and POD-only: MSVC 2010 rejects __try in a function that also
// holds objects needing unwinding.
template<bool Destroy> static bool ReadContainer(uintptr_t base,
                          unsigned __int64* sizeOut, void** headOut)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		unsigned __int64 nbuckets = *(unsigned __int64*)((Destroy ? KLIB_MEMBER(5, base, MovableSetTable_bucket_count_, 24) : KLIB_MEMBER(5, base, RootSetTable_bucket_count_, 24)));
		*sizeOut  = *(unsigned __int64*)((Destroy ? KLIB_MEMBER(5, base, MovableSetTable_size_, 32) : KLIB_MEMBER(5, base, RootSetTable_size_, 32)));
		void** buckets = *(void***)((Destroy ? KLIB_MEMBER(5, base, MovableSetTable_buckets_, 56) : KLIB_MEMBER(5, base, RootSetTable_buckets_, 56)));
		*headOut = NULL;
		if (buckets && nbuckets < MAX_SANE_BUCKETS)
			*headOut = buckets[nbuckets];
		else if (nbuckets >= MAX_SANE_BUCKETS)
			ok = false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// size and head are two separate loads, so a concurrent insert between them can
// fake a mismatch. The crash state is permanent (size stays high for good), so a
// second read that still disagrees is the real thing and a transient one is not.
template<bool Destroy> static bool ProbeContainer(const char* name, uintptr_t container,
                           unsigned int frame, bool quiet)
{
	unsigned __int64 size = 0;
	void* head = NULL;
	if (!ReadContainer<Destroy>(container, &size, &head))
		return false;

	bool broken = ((size != 0) != (head != NULL));
	if (!broken)
		return false;

	if (!ReadContainer<Destroy>(container, &size, &head))
		return false;
	if ((size != 0) == (head != NULL))
		return false;

	if (quiet)
		return false;

	std::ostringstream ss;
	ss << name << " INVARIANT BROKEN: size=" << size
	   << " head=0x" << std::hex << (uintptr_t)head << std::dec
	   << " frame=" << frame
	   << " lastInsertTid=" << (unsigned long)InterlockedCompareExchange(&s_lastInsertTid, 0, 0)
	   << DestroyListStatsSuffix();
	LogMsg(ss.str());
	return true;
}

void DestroyListProbeTick()
{
	// Main thread only, enforced here rather than assumed of the caller: the
	// frame counter and the log latch below are plain statics, and LogMsg's CRT
	// work is only safe on this thread.
	if (!IsMainThread())
		return;
	if (!s_gameWorld)
		return;

	static unsigned int frame = 0;
	frame++;

	// The read runs every frame — the corrupt state appears within a frame of the
	// crash, so a sampled probe would miss it. Only the logging is throttled, to
	// one line a minute: the state is persistent, so the frames after the first
	// detection would all say the same thing (if the game gets that far at all).
	static double lastLogSec = -1.0e9;
	double now = ElapsedSec();
	bool quiet = (now - lastLogSec < 60.0);

	bool logged = ProbeContainer<true>("destroyListOE", KLIB_MEMBER(5, s_gameWorld, GameWorld_destroyListOE, 0x680),
	                             frame, quiet);
	// Control container: the same shape, drained by the same function a few
	// hundred instructions earlier. If both break together the cause is wider
	// than one list.
	if (ProbeContainer<false>("killListPhase0", KLIB_MEMBER(5, s_gameWorld, GameWorld_killListPhase0, 0x7D0), frame, quiet))
		logged = true;

	if (logged)
		lastLogSec = now;
}

std::string DestroyListStatsSuffix()
{
	LONG main  = InterlockedCompareExchange(&s_insMain, 0, 0);
	LONG other = InterlockedCompareExchange(&s_insOther, 0, 0);

	std::ostringstream ss;
	ss << ", dlIns=" << main << "/" << other
	   << " dlDeferred=" << InterlockedCompareExchange(&s_deferred, 0, 0)
	   << " dlFlushed="  << InterlockedCompareExchange(&s_flushed, 0, 0)
	   << " dlDeferFull=" << InterlockedCompareExchange(&s_deferFull, 0, 0)
	   << " dlDedup=" << InterlockedCompareExchange(&s_dedup, 0, 0)
	   << " dlDropClear=" << InterlockedCompareExchange(&s_dropClear, 0, 0)
	   << " dlDeferEnt=" << InterlockedCompareExchange(&s_deferEnt, 0, 0)
	   << "/" << InterlockedCompareExchange(&s_deferNonEnt, 0, 0);

	// The return addresses prove the off-main call site, so print them the
	// first time any exist and never again.
	if (other > 0 && !s_ringPrinted)
	{
		s_ringPrinted = true;
		ss << " dlFrom=";
		LONG count = InterlockedCompareExchange(&s_ringNext, 0, 0);
		if (count > RING_SIZE)
			count = RING_SIZE;
		for (LONG i = 0; i < count; ++i)
		{
			uintptr_t ra = s_ring[i];
			if (i) ss << ",";
			ss << "0x" << std::hex
			   << (uintptr_t)((CoreGameBase() && ra > CoreGameBase()) ? ra - CoreGameBase() : ra)
			   << std::dec;
		}
	}
	return ss.str();
}

void SetDestroyListDefer(bool enabled)
{
	s_deferEnabled = enabled;
}

void DestroyListFlushDeferred()
{
	// Main thread only: the whole point is that the real insert happens on the
	// thread that drains the container.
	if (!IsMainThread() || !deferCSInitialized || !orig_destroyListInsert)
		return;

	// Batched so the lock is held for a pointer copy and nothing else. The
	// original is called with deferCS released, because it runs engine code
	// (a virtual type-name query, setVisible, the hash insert) that must never
	// run under a mod lock a background thread can be waiting on.
	//
	// The queue is drained from the end: order is irrelevant to a hash-set
	// insert, and popping the tail keeps the bookkeeping O(1).
	//
	// Bounded: at most FLUSH_BATCHES batches per frame, leftovers next frame.
	// Only a pathological producer can reach the bound (the worst session on
	// record queued 129 inserts in total), but the cost of this loop should not
	// scale with a queue the mod does not control.
	for (int pass = 0; pass < FLUSH_BATCHES; ++pass)
	{
		DeferredInsert batch[FLUSH_BATCH];
		int taken = 0;

		EnterCriticalSection(&deferCS);
		while (taken < FLUSH_BATCH && s_deferCount > 0)
			batch[taken++] = s_defer[--s_deferCount];
		LeaveCriticalSection(&deferCS);

		if (taken == 0)
			return;

		for (int i = 0; i < taken; ++i)
		{
			// Dedup within the batch. The push path already refuses a pointer
			// the queue holds, so this is unreachable in practice; it is here
			// because inserting one pointer twice is exactly the failure the
			// dedup exists to prevent, and 32 comparisons cost nothing.
			bool dup = false;
			for (int j = 0; j < i; ++j)
			{
				if (batch[j].movable == batch[i].movable)
				{
					dup = true;
					break;
				}
			}
			if (dup)
			{
				InterlockedIncrement(&s_dedup);
				continue;
			}
			orig_destroyListInsert(batch[i].gameWorld, batch[i].movable);
			InterlockedIncrement(&s_flushed);
		}
	}
}

void DestroyListDropDeferred()
{
	// World clear: everything queued names an object the clear is about to
	// destroy, so replaying it would insert a freed pointer into a container
	// the clear has just emptied. Dropping loses nothing — the clear tears the
	// scene down wholesale, which is what the queued inserts were asking for.
	if (!deferCSInitialized)
		return;

	EnterCriticalSection(&deferCS);
	int dropped = s_deferCount;
	s_deferCount = 0;
	LeaveCriticalSection(&deferCS);

	if (dropped > 0)
		InterlockedExchangeAdd(&s_dropClear, (LONG)dropped);
}

__int64 __fastcall hook_destroyListInsert(void* gameWorld, void* movable)
{
	InterlockedExchange(&s_lastInsertTid, (LONG)GetCurrentThreadId());

	if (IsMainThread())
	{
		InterlockedIncrement(&s_insMain);

		// Deferral must not lose the set's own dedup. Vanilla's insert of an
		// object the set already holds is a no-op; with a queue in the way, a
		// main-thread insert of an object still queued would land in the set
		// now, this frame's drain would destroy it, and the flush would then
		// replay a freed pointer. So a main-thread insert claims the object:
		// anything queued for it is removed before the original runs.
		if (s_deferEnabled && deferCSInitialized && movable)
		{
			EnterCriticalSection(&deferCS);
			int removed = RemoveQueuedLocked(movable);
			LeaveCriticalSection(&deferCS);
			if (removed > 0)
				InterlockedExchangeAdd(&s_dedup, (LONG)removed);
		}
	}
	else
	{
		InterlockedIncrement(&s_insOther);
		// The ring keeps the first RING_SIZE off-main-thread call sites; later
		// ones wrap over the oldest. Either way the slot index is unique, so two
		// threads never write the same entry.
		LONG slot = InterlockedIncrement(&s_ringNext) - 1;
		// Unsigned: slot is a LONG and wraps negative after 2^31 inserts, and a
		// negative operand to % would index before the array.
		s_ring[((unsigned long)slot) % RING_SIZE] = (uintptr_t)_ReturnAddress();

		// The mitigation (destroy_list_defer.h): queue instead of inserting, so that every
		// write to destroyListOE happens on the thread that drains it. The main
		// thread performs this insert from DestroyListFlushDeferred on the next
		// frame; the object stays alive until a drain reaches it either way.
		if (s_deferEnabled && deferCSInitialized && orig_destroyListInsert)
		{
			// Which branch of the original this insert would have taken. The
			// "Entity" branch also calls sub_140449240, which de-registers the
			// object from the loader's queued (+320/+328) and preloaded
			// (+288/+296) lists; deferring the insert defers that too, by one
			// frame, while the object stays alive. Counted, not worked around.
			if (MovableTypeIsEntity(movable))
				InterlockedIncrement(&s_deferEnt);
			else
				InterlockedIncrement(&s_deferNonEnt);

			bool queued = false;
			bool already = false;
			EnterCriticalSection(&deferCS);
			// Dedup: the set the flush inserts into holds each pointer once, so
			// a second deferral of the same object must not become a second
			// insert. Keeping the queue duplicate-free also means no batch of
			// the flush can ever hold one pointer twice.
			int collapsed = RemoveQueuedLocked(movable);
			if (collapsed > 0)
			{
				// Put it back once: this call still wants the object destroyed.
				s_defer[s_deferCount].gameWorld = gameWorld;
				s_defer[s_deferCount].movable   = movable;
				s_deferCount++;
				queued  = true;
				already = true;
			}
			else if (s_deferCount < DEFER_CAP)
			{
				s_defer[s_deferCount].gameWorld = gameWorld;
				s_defer[s_deferCount].movable   = movable;
				s_deferCount++;
				queued = true;
			}
			LeaveCriticalSection(&deferCS);

			if (already)
				InterlockedExchangeAdd(&s_dedup, (LONG)collapsed);

			if (queued)
			{
				InterlockedIncrement(&s_deferred);
				// The original's return value is the address of its own dead
				// stack temporary and no call site reads it (destroy_list_defer.h).
				return 0;
			}
			// Queue full: fall through to the racy original rather than drop
			// the object, which would leak it and leave it visible.
			InterlockedIncrement(&s_deferFull);
		}
	}

	if (!orig_destroyListInsert)
		return 0;
	return orig_destroyListInsert(gameWorld, movable);
}
