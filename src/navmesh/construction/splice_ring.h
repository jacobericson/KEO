// splice_ring.h - The lock-free record ring between the wall progress detour (any thread) and the
// main-thread drain: 64 slots, overwrite-oldest, and every index accounted once by the drain.
#ifndef KEO_SPLICE_RING_H
#define KEO_SPLICE_RING_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace navmesh {

const int  SPLICE_RING_SLOTS        = 64;   // a power of two
const LONG SPLICE_SEQ_EMPTY         = -1;   // never written since init
const LONG SPLICE_SEQ_WRITING       = -2;   // claimed by a producer, being filled
const int  SPLICE_RING_STALL_PASSES = 2;    // drain passes an unpublished index may hold the cursor

// seq: SPLICE_SEQ_EMPTY, SPLICE_SEQ_WRITING, or the index of the record the slot holds.
struct SpliceRingSlot { volatile LONG seq; float box[6]; };
struct SpliceRing
{
	SpliceRingSlot slot[SPLICE_RING_SLOTS];
	volatile LONG  written;      // indices taken from the counter, all producers
	volatile LONG  claimFailed;  // producers whose slot was held or newer, record dropped (diagnostic)
	volatile LONG  lost;         // indices the drain passed without a record: overrun, overwritten, raced or abandoned
	volatile LONG  overwritten;  // of lost: a later lap had published over the slot
	volatile LONG  readPub;      // the drain's cursor, published at the end of each drain
	LONG           read;         // the drain's cursor; main thread only
	LONG           stallAt;      // the cursor the last stopped pass stopped at; main thread only
	int            stallPasses;  // consecutive passes stopped there; main thread only
};

// Main thread, before any producer can run: every seq to SPLICE_SEQ_EMPTY, every counter and
// readPub to 0, stallAt to -1.
void SpliceRingInit(SpliceRing* r);
// Any thread, no lock, no allocation, no loop. Takes an index from the counter, claims its slot by
// one compare-exchange from a published (or empty) value no newer than the index to WRITING, fills
// it and publishes the index last. A failed claim drops the record and counts claimFailed.
void SpliceRingPush(SpliceRing* r, const float box[6]);
enum SplicePush { SPLICE_PUSH_TAKEN = 0, SPLICE_PUSH_FULL, SPLICE_PUSH_CLAIM_FAILED };
// Any thread, no lock, no allocation, no loop. FULL, taking no index, while the producers are a
// full ring ahead of the drain's published cursor; otherwise SpliceRingPush's claim and publish:
// TAKEN, or CLAIM_FAILED when the claim fails (counted in claimFailed; the drain later passes the
// index as lost).
SplicePush SpliceRingTryPush(SpliceRing* r, const float box[6]);
// Main thread: copies up to max records in index order and returns the count; every index below
// written is accounted once, as drained or as lost.
int  SpliceRingDrain(SpliceRing* r, float (*out)[6], int max);

} // namespace navmesh

#endif
