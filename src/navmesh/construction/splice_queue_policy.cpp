// splice_queue_policy.cpp - The deferred wall splice's decisions.
#include "navmesh/construction/splice_queue_policy.h"
#include <float.h>
#include <string.h>

namespace navmesh {

bool SpliceBelow(float progress, float totalMats, float pathThreshold)
{
	const float threshold = totalMats * pathThreshold;
	return progress < threshold;
}

bool SpliceRecordDue(int shareType, bool belowBefore, float progressAfter, float totalMats,
                     float pathThreshold, float amount, bool hasPhysical)
{
	return shareType == SPLICE_HAND_NULL_ITEM
	    && belowBefore
	    && progressAfter >= totalMats * pathThreshold
	    && amount != FLT_MAX
	    && hasPhysical;
}

void SpliceBoxMerge(float box[6], const float add[6])
{
	for (int a = 0; a < 3; ++a)
	{
		float lo = box[a] - box[a + 3];
		float hi = box[a] + box[a + 3];
		const float addLo = add[a] - add[a + 3];
		const float addHi = add[a] + add[a + 3];
		if (addLo < lo) lo = addLo;
		if (addHi > hi) hi = addHi;
		box[a]     = (lo + hi) * 0.5f;
		box[a + 3] = (hi - lo) * 0.5f;
	}
}

bool SpliceCoalesce(float box[6], int cellX, int cellY, const float add[6], int addCellX, int addCellY)
{
	if (cellX != addCellX || cellY != addCellY)
		return false;
	float merged[6];
	for (int i = 0; i < 6; ++i)
		merged[i] = box[i];
	SpliceBoxMerge(merged, add);
	// Full size on x (axis 0) and on z (axis 2), as the placement coalescer measures it.
	if (merged[3] * 2.0f >= SPLICE_MERGE_EXTENT || merged[5] * 2.0f >= SPLICE_MERGE_EXTENT)
		return false;
	for (int i = 0; i < 6; ++i)
		box[i] = merged[i];
	return true;
}

SpliceStep SpliceDecide(const SpliceGate& g, int ticksWaited)
{
	if (g.cellGone)
		return SQ_DROP;
	if (!g.worldOk)
		return SQ_WAIT;
	if (g.mainCount != 0 || g.backCount != 0 || !g.queuesClear || !g.cellsReady)
		return ticksWaited >= SPLICE_WAIT_CAP_TICKS ? SQ_REQUEUE : SQ_WAIT;
	return SQ_ISSUE;
}

bool SpliceArmed(bool detourInstalled, bool nopWritten)
{
	return detourInstalled && nopWritten;
}

const unsigned __int64 kWallSpliceLeadRva = 0x5596CB;
const unsigned __int64 kWallSpliceCallRva = 0x5596E4;

// movss xmm0,[9999.0]; comiss xmm0,xmm6; jbe +0x11; mov rcx,pauseState.navmesh;
// lea rdx,[rsp+20h]; call NavMesh::generate(Aabb) -- the call is the last five bytes.
const unsigned char kWallSpliceLeadBytes[30] =
{
	0xF3, 0x0F, 0x10, 0x05, 0x49, 0x93, 0x15, 0x01,
	0x0F, 0x2F, 0xC6,
	0x76, 0x11,
	0x48, 0x8B, 0x0D, 0x81, 0x9E, 0xBD, 0x01,
	0x48, 0x8D, 0x54, 0x24, 0x20,
	0xE8, 0x2F, 0xBC, 0xAE, 0xFF
};

// A single five-byte NOP: nothing after the call reads rax, rcx or rdx before writing them.
const unsigned char kWallSpliceNop[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };

bool WallSpliceLeadMatches(const unsigned char* actual)
{
	return actual != 0 && memcmp(actual, kWallSpliceLeadBytes, kWallSpliceLeadLen) == 0;
}

} // namespace navmesh
