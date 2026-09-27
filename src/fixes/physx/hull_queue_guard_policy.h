#ifndef KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_POLICY_H

#include <stddef.h>
#include <stdint.h>

// Which entries of PhysicsInterface::hullsToDestroy's main-thread list are a
// second queueing of an object the queue already owns.
//
// hullsToDestroy is a MessageChain: the main thread appends to its main list,
// PhysicsActual::updateUT flushes the main list onto the back list, and the
// physics thread (threadJunkPreBT) deletes every back entry, skipping NULLs,
// and zeroes the back count without touching the buffer. updateUT runs only
// while the physics thread is idle, so at its entry the lists are quiescent
// and the back buffer still holds the batch the physics thread last deleted.
//
// At that point an entry p of the main list is a duplicate when
//   - p is earlier in the same main list (two pushes, no free between them);
//   - p is still in an unconsumed back list (queued, alive, not yet freed);
//   - p was in the batch the physics thread has since deleted, and p was not
//     pushed onto hullsToMake since: the push reached an object before its
//     free, and the entry now names freed memory.
// A new object allocated at a freed address is registered through
// hullsToMake before anything can queue it for destruction, so a p made
// since the batch is a legitimate new object and is kept. The judge never
// dereferences an entry.
//
// No Windows header and no game pointer here, so a host test can fabricate
// the lists and drive every arm.

// PhysicsInterface MessageChains: vtable, main lektor at +0x08, back lektor
// at +0x20; a lektor is vtable, count +0x08, capacity +0x0C, data +0x10.
const size_t OFF_HQG_MAKE_MAIN_COUNT     = 0x1B0;   // hullsToMake (+0x1A0)
const size_t OFF_HQG_MAKE_MAIN_DATA      = 0x1B8;
const size_t OFF_HQG_DESTROY_MAIN_COUNT  = 0x200;   // hullsToDestroy (+0x1F0)
const size_t OFF_HQG_DESTROY_MAIN_DATA   = 0x208;
const size_t OFF_HQG_DESTROY_BACK_COUNT  = 0x218;
const size_t OFF_HQG_DESTROY_BACK_DATA   = 0x220;

// A list longer than this is not one the game built; the frame is not judged.
const unsigned int HQG_MAX_LIST = 1u << 20;

enum HqgDup
{
	HQG_DUP_NONE = 0,
	HQG_DUP_IN_LIST,     // an earlier entry of the same main list
	HQG_DUP_PENDING,     // in the back list the physics thread has not consumed
	HQG_DUP_FREED        // in the batch the physics thread deleted, not made since
};

enum HqgOutcome
{
	HQG_OUT_EMPTY = 0,     // the main list is empty
	HQG_OUT_JUDGED,
	HQG_OUT_SKIP_STATE,    // the back list is not what the last flush left
	HQG_OUT_SKIP_LIST,     // a count or buffer is not believable
	HQG_OUT_SKIP_OVERFLOW, // more addresses than the table holds
	HQG_OUT_RACED          // the main list moved while an entry was dropped
};

struct HqgSlot
{
	uintptr_t key;
	unsigned  epoch;
	unsigned  flags;
};

struct HqgState
{
	HqgSlot*         slots;
	unsigned         mask;
	unsigned         epoch;
	bool             haveRec;    // a flush has been recorded
	const uintptr_t* recData;    // back buffer and count right after that flush
	unsigned         recCount;
};

struct HqgLists
{
	uintptr_t*       destroyMain;
	unsigned         destroyMainCount;
	const uintptr_t* destroyBack;
	unsigned         destroyBackCount;
	const uintptr_t* makeMain;
	unsigned         makeMainCount;
};

struct HqgFinding
{
	unsigned  index;
	uintptr_t ptr;
	HqgDup    kind;
};

// Drops entry `index`, which must still hold `expected`. False: the list
// moved under the call, and judging stops.
typedef bool (*HqgNullFn)(void* ctx, unsigned index, uintptr_t expected);

struct HqgResult
{
	HqgOutcome outcome;
	unsigned   scanned;      // non-NULL main entries looked at
	unsigned   dups;         // == inList + pending + freed
	unsigned   inList;
	unsigned   pending;
	unsigned   freed;
	unsigned   keptRemade;   // in the deleted batch, but made again since: kept
	unsigned   nulled;       // act mode: entries dropped
	unsigned   batchCount;   // size of the batch known deleted (0: none)
	unsigned   pendingCount; // size of the unconsumed back list (0: none)
	unsigned   findings;     // entries written to out (<= outCap)
};

// slotCount: a power of two. The slots need not be zeroed.
void HqgInit(HqgState* s, HqgSlot* slots, unsigned slotCount);

// Judges the main list. With act, each duplicate is handed to nullFn as it
// is found; without it nothing is written. The first outCap duplicates are
// reported in out.
HqgResult HqgJudge(HqgState* s, const HqgLists* in, bool act, HqgNullFn nullFn, void* ctx,
                   HqgFinding* out, unsigned outCap);

// After the flush: the back list as the physics thread will receive it.
void HqgRecord(HqgState* s, const uintptr_t* backData, unsigned backCount);

// Drops the record, as before the first flush: the next judge treats the
// back list as pending and knows no deleted batch.
void HqgForget(HqgState* s);

// Which duplicates the guard drops: all of them.
inline bool HqgDupDrops(HqgDup d) { return d != HQG_DUP_NONE; }

const char* HqgDupName(HqgDup d);

// Last two push callers of one address, packed (last in the high half).
inline unsigned __int64 HqgPushCallers(unsigned __int64 old, unsigned rva)
{
	return ((unsigned __int64)rva << 32) | (old >> 32);
}
inline unsigned HqgLastCaller(unsigned __int64 w) { return (unsigned)(w >> 32); }
inline unsigned HqgPrevCaller(unsigned __int64 w) { return (unsigned)(w & 0xFFFFFFFFu); }

#endif // KENSHI_ZONE_OPT_FIXES_HULL_QUEUE_GUARD_POLICY_H
