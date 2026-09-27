#ifndef KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_POLICY_H

// Decision logic for the cross-section un-stitch probe (unstitch_probe.h).
// Pure arithmetic: no game headers, no KenshiLib, host-testable.
//
// The native un-stitch walks a dying graph instance's streaming sets and, for
// every graph connection in a set that names this instance's section, reads
// the opposite instance's node map at the connection's recorded opposite node
// index. It guards that read with one test only -- "is the map non-empty" --
// and never compares the index against the map's size. These predicates say
// which of those reads would land outside the map.

enum UnstitchNodeVerdict
{
	UNSTITCH_NODE_IN_BOUNDS = 0, // the read lands inside the map (or is never made)
	UNSTITCH_NODE_NEGATIVE,      // index below zero: reads before the map
	UNSTITCH_NODE_PAST_END       // index at or past the map's element count
};

// nodeMapSize <= 0 answers IN_BOUNDS on purpose: the native code short-circuits
// on an empty map and never performs the indexed read, so any index is
// harmless there and a verdict would name a fault that cannot happen.
//
// A negative index is not a sentinel to this code. The only -1 test in the
// native walk is on the *value read back* from the map; the index itself is
// used unchecked, so a negative one reads before the array exactly as an
// oversized one reads past it. The two verdicts differ only in which counter
// they feed, and both mean a wild read.
UnstitchNodeVerdict UnstitchClassifyNodeIndex(int nodeIndex, int nodeMapSize);

// True when the verdict names a read the probe should report.
bool UnstitchVerdictIsRow(UnstitchNodeVerdict v);

// Upper bounds on the two array lengths the walk trusts. They exist so a
// garbage or torn header cannot turn the probe into an unbounded loop; a
// length outside them is reported as implausible, never walked and never
// silently dropped.
const int UNSTITCH_MAX_SETS  = 4096;
const int UNSTITCH_MAX_CONNS = 1048576;

bool UnstitchSetCountPlausible(int setCount);
bool UnstitchConnCountPlausible(int connCount);

enum UnstitchSetAction
{
	UNSTITCH_SET_SKIP_NOT_OURS,    // the set does not name this instance's section
	UNSTITCH_SET_SKIP_IMPLAUSIBLE, // the connection count is not a length we trust
	UNSTITCH_SET_WALK              // walk this set's connections
};

// Mirrors the native walk's own set filter: it compares the set's m_thisUid
// against the dying instance's section uid and skips everything else.
UnstitchSetAction UnstitchClassifySet(int thisUid, int ownUid, int connCount);


// ---------------------------------------------------------------------------
// Host-side walk driver
// ---------------------------------------------------------------------------
// The same iteration the detour performs, expressed over plain arrays so a
// synthesised streaming set can be driven through it on the host. The detour
// reads its records out of game memory and this driver out of these stubs;
// both reach the per-record decision through the predicates above.

struct UnstitchSetStub
{
	int thisUid;
	int oppositeUid;
	bool oppositeResolved;    // the collection had a live instance for oppositeUid
	int oppositeNodeMapSize;  // that instance's node-map element count
	const int* oppositeNodeIndices;  // one per connection, as recorded
	int connCount;
};

struct UnstitchWalkTally
{
	int setsSeen;
	int setsMatched;      // named this instance's section
	int oppositeResolved; // and had a live opposite instance
	int connsExamined;
	int rows;             // reads that would land outside the opposite node map
	int negativeRows;     // of those, indices below zero
	int implausible;      // lengths refused by the bounds above
};

// setCount outside UNSTITCH_MAX_SETS leaves every field zero except
// implausible, which counts the refused array once.
UnstitchWalkTally UnstitchWalkStubs(int ownUid, const UnstitchSetStub* sets, int setCount);

#endif // KENSHI_ZONE_OPT_FIXES_UNSTITCH_PROBE_POLICY_H
