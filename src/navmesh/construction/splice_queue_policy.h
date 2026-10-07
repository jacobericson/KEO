// splice_queue_policy.h - The deferred wall splice's decisions: when a progress call records a
// crossing, how two boxes coalesce, and when a pending box is issued. Pure: no Windows, KenshiLib
// or game header; any thread.
#ifndef KEO_SPLICE_QUEUE_POLICY_H
#define KEO_SPLICE_QUEUE_POLICY_H

namespace navmesh {

const int   SPLICE_HAND_NULL_ITEM = 11;     // shareBuildStateOfAnother.type of a wall that owns its state
const float SPLICE_MERGE_EXTENT   = 400.0f; // a coalesced box stays under this on x and on z
const int   SPLICE_WAIT_CAP_TICKS = 600;    // ticks before a waiting box is re-queued and counted

// Vanilla's own test: progress below totalMats * pathThreshold, in single precision.
bool SpliceBelow(float progress, float totalMats, float pathThreshold);
// The crossing block vanilla runs (and so the record): the wall owns its build state, was below
// before the call, is at or over the threshold after it (an ordered compare: a NaN progress never
// records), the amount is not FLT_MAX, and it has a physical.
bool SpliceRecordDue(int shareType, bool belowBefore, float progressAfter, float totalMats,
                     float pathThreshold, float amount, bool hasPhysical);

// Boxes are Ogre::Aabb's layout: centre x, y, z, then half size x, y, z.
void SpliceBoxMerge(float box[6], const float add[6]);
// Merges add into box when both centres lie in the same cell and the union stays under
// SPLICE_MERGE_EXTENT on x and on z; false (box unchanged) otherwise.
bool SpliceCoalesce(float box[6], int cellX, int cellY, const float add[6], int addCellX, int addCellY);

struct SpliceGate
{
	int  mainCount;     // hullsToChangeGroup.mainThreadData.count
	int  backCount;     // hullsToChangeGroup.backThreadData.count
	bool queuesClear;   // PhysicsInterface::queuesAreClearMT
	bool worldOk;       // no save load, reset gate, NavMesh::stop or transition
	bool cellsReady;    // every touched cell accessible, not private, and ready by the original isContentPending
	bool cellGone;      // a touched cell has unloaded since the record
};
enum SpliceStep { SQ_ISSUE = 0, SQ_WAIT, SQ_REQUEUE, SQ_DROP };
// In this order: a gone cell drops; a world not ok waits without aging; a non-zero count, queues
// not clear or a cell not ready waits, and re-queues at SPLICE_WAIT_CAP_TICKS; else issue.
SpliceStep SpliceDecide(const SpliceGate& g, int ticksWaited);

// The splice is armed only when both halves are in: without the NOP vanilla still splices, and
// without the detour nothing would.
bool SpliceArmed(bool detourInstalled, bool nopWritten);

// The call site: the amount test, the navmesh load and the call itself.
extern const unsigned __int64 kWallSpliceLeadRva;   // 0x5596CB
extern const unsigned __int64 kWallSpliceCallRva;   // 0x5596E4
const int kWallSpliceLeadLen = 30;
extern const unsigned char kWallSpliceLeadBytes[30];
extern const unsigned char kWallSpliceNop[5];       // 0F 1F 44 00 00
bool WallSpliceLeadMatches(const unsigned char* actual);

} // namespace navmesh

#endif
