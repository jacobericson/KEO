#ifndef KEO_FIXES_UNSTITCH_GUARD_POLICY_H
#define KEO_FIXES_UNSTITCH_GUARD_POLICY_H

#include "fixes/stitch/unstitch_probe_policy.h"

// Decision logic for the cross-section un-stitch bounds guard. Pure
// arithmetic: no game headers, no KenshiLib, host-testable.
//
// The native walk resolves a recorded connection's opposite node index through
// the opposite instance's node map with one test only -- "is the map empty" --
// and then uses the value it read back as an index into that instance's
// instanced-node array. Neither the recorded index nor the value read back is
// compared against the array it indexes. These predicates are those two
// missing comparisons.

enum UnstitchConnAction
{
	UNSTITCH_CONN_PROCESS = 0,  // hand the record to the native un-stitch step
	UNSTITCH_CONN_SKIP_OOB      // the recorded index misses the opposite node map
};

// The -1 the native code tests is a test on the value read *back* from the
// map, never on the index, so it is untouched by this: a record whose map slot
// holds -1 still reaches the native skip. This is a second, earlier test on the
// index, which nothing in the native walk bounds.
UnstitchConnAction UnstitchGuardConnAction(int nodeIndex, int nodeMapSize);

enum UnstitchMapValueAction
{
	UNSTITCH_MAP_USE = 0,         // index the instanced nodes with it
	UNSTITCH_MAP_NATIVE_SENTINEL, // -1: the native walk's own skip, unchanged
	UNSTITCH_MAP_SKIP_OUT_OF_RANGE
};

// The value read back out of the node map indexes the opposite instance's
// instanced nodes, and that second indexed load is the one that faults. -1 is
// the native "no instanced node yet" sentinel and keeps its meaning here: it
// is its own answer, never an index and never a guard action. Any other value
// outside the array is.
UnstitchMapValueAction UnstitchGuardMapValue(int mapValue, int instancedNodeCount);

// ---------------------------------------------------------------------------
// Host-side walk driver
// ---------------------------------------------------------------------------
// The same set/connection iteration the guard performs, over synthesised
// records, recording which connections it handed on so a test can say that the
// offending one was dropped and the rest survived in order.

struct UnstitchGuardTally
{
	int setsSeen;
	int setsMatched;
	int oppositeResolved;
	int connsExamined;
	int connsProcessed;   // handed to the un-stitch step
	int connsSkipped;     // the recorded index missed the opposite node map
	int implausible;
};

// processedOut receives the connection index of every record handed on, in
// order, up to processedCap entries.
UnstitchGuardTally UnstitchGuardWalkStubs(int ownUid, const UnstitchSetStub* sets, int setCount,
                                          int* processedOut, int processedCap);

#endif // KEO_FIXES_UNSTITCH_GUARD_POLICY_H
