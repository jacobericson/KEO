// The deferred wall splice's rules and its record ring, single-threaded.
#include <cstdio>
#include <cfloat>
#include <cstring>
#include "navmesh/construction/splice_queue_policy.h"
#include "navmesh/construction/splice_ring.h"

#include "check.h"

using namespace navmesh;

// The call site as the disassembly prints it, typed here independently of the policy's constant.
static const unsigned char kIdbLead[30] =
{
	0xF3, 0x0F, 0x10, 0x05, 0x49, 0x93, 0x15, 0x01, 0x0F, 0x2F, 0xC6, 0x76, 0x11, 0x48, 0x8B,
	0x0D, 0x81, 0x9E, 0xBD, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8, 0x2F, 0xBC, 0xAE, 0xFF
};
static const unsigned char kIdbNop[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };

static bool Near(float a, float b)
{
	const float d = a - b;
	return d < 0.0001f && d > -0.0001f;
}

static void CheckRecord()
{
	// 10 * 0.3f is 3.0000001 in double but rounds to 3.0f in single precision, as the mulss does:
	// progress 3.0f is therefore not below, where a double product would call it below.
	Check(!SpliceBelow(3.0f, 10.0f, 0.3f) && SpliceBelow(2.9999998f, 10.0f, 0.3f)
	      && SpliceBelow(9.49f, 10.0f, 0.95f) && !SpliceBelow(9.5f, 10.0f, 0.95f)
	      && SpliceBelow(0.0f, 4.0f, 0.2f),
	      "below: vanilla's single-precision product");

	const int owner = SPLICE_HAND_NULL_ITEM;
	Check(SpliceRecordDue(owner, true, 8.0f, 10.0f, 0.8f, 2.0f, true), "record: an owner crossing records");
	Check(!SpliceRecordDue(0, true, 8.0f, 10.0f, 0.8f, 2.0f, true), "record: a follower never records");
	Check(!SpliceRecordDue(owner, false, 9.0f, 10.0f, 0.8f, 1.0f, true), "record: already over before does not record");
	Check(!SpliceRecordDue(owner, true, 7.0f, 10.0f, 0.8f, 1.0f, true), "record: still below after does not record");
	Check(!SpliceRecordDue(owner, true, 10.0f, 10.0f, 0.8f, FLT_MAX, true), "record: FLT_MAX never records");
	Check(SpliceRecordDue(owner, true, 10.0f, 10.0f, 0.8f, 9999.0f, true), "record: 9999 records");
	Check(!SpliceRecordDue(owner, true, 8.0f, 10.0f, 0.8f, 2.0f, false), "record: no physical does not record");
}

static void CheckBoxes()
{
	// [0,10] x [0,2] x [0,4] merged with [5,25] x [-2,1] x [3,7]: [0,25] x [-2,2] x [0,7].
	float box[6] = { 5.0f, 1.0f, 2.0f, 5.0f, 1.0f, 2.0f };
	const float add[6] = { 15.0f, -0.5f, 5.0f, 10.0f, 1.5f, 2.0f };
	SpliceBoxMerge(box, add);
	Check(Near(box[0], 12.5f) && Near(box[1], 0.0f) && Near(box[2], 3.5f)
	      && Near(box[3], 12.5f) && Near(box[4], 2.0f) && Near(box[5], 3.5f),
	      "merge: the union's centre and half size");

	float a[6] = { 100.0f, 0.0f, 100.0f, 50.0f, 10.0f, 50.0f };
	const float b[6] = { 200.0f, 0.0f, 150.0f, 50.0f, 10.0f, 50.0f };  // union 200 x 150
	Check(SpliceCoalesce(a, 3, 4, b, 3, 4) && Near(a[0], 150.0f) && Near(a[3], 100.0f)
	      && Near(a[2], 125.0f) && Near(a[5], 75.0f),
	      "coalesce: same cell under 400 merges");

	float c[6] = { 100.0f, 0.0f, 100.0f, 50.0f, 10.0f, 50.0f };
	Check(!SpliceCoalesce(c, 3, 4, b, 4, 4) && Near(c[0], 100.0f) && Near(c[3], 50.0f),
	      "coalesce: another cell does not merge");

	float d[6] = { 100.0f, 0.0f, 100.0f, 50.0f, 10.0f, 50.0f };
	const float farX[6] = { 400.0f, 0.0f, 100.0f, 50.0f, 10.0f, 50.0f };  // x from 50 to 450: 400
	Check(!SpliceCoalesce(d, 1, 1, farX, 1, 1) && Near(d[0], 100.0f) && Near(d[3], 50.0f),
	      "coalesce: 400 or more on x does not merge");

	float e[6] = { 100.0f, 0.0f, 100.0f, 50.0f, 10.0f, 50.0f };
	const float farZ[6] = { 100.0f, 0.0f, 420.0f, 50.0f, 10.0f, 50.0f };  // z from 50 to 470: 420
	Check(!SpliceCoalesce(e, 1, 1, farZ, 1, 1) && Near(e[2], 100.0f) && Near(e[5], 50.0f),
	      "coalesce: 400 or more on z does not merge");
}

static SpliceGate ClearGate()
{
	SpliceGate g;
	g.mainCount = 0;
	g.backCount = 0;
	g.queuesClear = true;
	g.worldOk = true;
	g.cellsReady = true;
	g.cellGone = false;
	return g;
}

static void CheckDecide()
{
	SpliceGate g = ClearGate();
	g.cellGone = true;
	g.worldOk = false;
	g.cellsReady = false;
	Check(SpliceDecide(g, 3) == SQ_DROP, "decide: a gone cell drops even while the world is not ok");

	g = ClearGate();
	g.worldOk = false;
	Check(SpliceDecide(g, SPLICE_WAIT_CAP_TICKS * 10) == SQ_WAIT, "decide: a world not ok waits without re-queue");

	g = ClearGate();
	g.mainCount = 2;
	Check(SpliceDecide(g, 1) == SQ_WAIT, "decide: a main count waits");
	g = ClearGate();
	g.backCount = 1;
	Check(SpliceDecide(g, 1) == SQ_WAIT, "decide: a back count waits");
	g = ClearGate();
	g.queuesClear = false;
	Check(SpliceDecide(g, 1) == SQ_WAIT, "decide: queues not clear wait");
	g = ClearGate();
	g.cellsReady = false;
	Check(SpliceDecide(g, 1) == SQ_WAIT, "decide: a cell not ready waits");

	g = ClearGate();
	g.backCount = 1;
	Check(SpliceDecide(g, SPLICE_WAIT_CAP_TICKS - 1) == SQ_WAIT
	      && SpliceDecide(g, SPLICE_WAIT_CAP_TICKS) == SQ_REQUEUE,
	      "decide: the cap re-queues");

	g = ClearGate();
	Check(SpliceDecide(g, 0) == SQ_ISSUE && SpliceDecide(g, SPLICE_WAIT_CAP_TICKS * 2) == SQ_ISSUE,
	      "decide: all clear issues");

	Check(SpliceArmed(true, true), "armed: both halves arm");
	Check(!SpliceArmed(true, false), "armed: the detour alone does not");
	Check(!SpliceArmed(false, true), "armed: the NOP alone does not");
}

static void CheckSite()
{
	Check(kWallSpliceLeadLen == 30 && memcmp(kWallSpliceLeadBytes, kIdbLead, 30) == 0
	      && WallSpliceLeadMatches(kIdbLead),
	      "lead: the IDB bytes match");
	Check(kWallSpliceCallRva - kWallSpliceLeadRva == 25, "lead: the call is the lead's last five bytes");

	bool refused = true;
	for (int i = 0; i < 30; ++i)
	{
		unsigned char changed[30];
		memcpy(changed, kIdbLead, 30);
		changed[i] ^= 0x01;
		refused = refused && !WallSpliceLeadMatches(changed);
	}
	Check(refused && !WallSpliceLeadMatches(NULL), "lead: a changed byte refuses");

	Check(memcmp(kWallSpliceNop, kIdbNop, 5) == 0, "nop: five bytes 0F 1F 44 00 00");
}

static void PushN(SpliceRing* r, int count, int first)
{
	for (int k = 0; k < count; ++k)
	{
		const float f = (float)(first + k);
		const float box[6] = { f, f + 0.5f, f + 1.0f, 1.0f, 2.0f, 3.0f };
		SpliceRingPush(r, box);
	}
}

static SpliceRing s_ring;
static float s_out[128][6];

static void CheckRing()
{
	SpliceRingInit(&s_ring);
	Check(SpliceRingDrain(&s_ring, s_out, 64) == 0 && s_ring.read == 0 && s_ring.lost == 0,
	      "ring: an empty ring drains nothing");
	bool empty = true;
	for (int i = 0; i < SPLICE_RING_SLOTS; ++i)
		empty = empty && s_ring.slot[i].seq == SPLICE_SEQ_EMPTY;
	Check(empty && s_ring.stallAt == -1, "ring: init marks every slot empty");

	SpliceRingInit(&s_ring);
	PushN(&s_ring, 1, 7);
	Check(SpliceRingDrain(&s_ring, s_out, 64) == 1 && Near(s_out[0][0], 7.0f) && Near(s_out[0][2], 8.0f)
	      && Near(s_out[0][5], 3.0f) && s_ring.read == 1 && s_ring.lost == 0,
	      "ring: push then drain returns it");

	SpliceRingInit(&s_ring);
	PushN(&s_ring, 5, 0);
	bool order = SpliceRingDrain(&s_ring, s_out, 64) == 5;
	for (int i = 0; i < 5; ++i)
		order = order && Near(s_out[i][0], (float)i);
	Check(order, "ring: order is kept");

	// 70 pushes: the last six overwrite slots 0-5, so indices 0-5 are lost.
	SpliceRingInit(&s_ring);
	PushN(&s_ring, 70, 0);
	const int got = SpliceRingDrain(&s_ring, s_out, 64);
	Check(got == 64 && s_ring.lost == 6 && s_ring.read == 70 && Near(s_out[0][0], 6.0f)
	      && Near(s_out[63][0], 69.0f),
	      "ring: an overrun counts the lost records");
	int drained = got;

	// The same ring, one more index whose producer has not published.
	PushN(&s_ring, 1, 70);
	s_ring.slot[70 & (SPLICE_RING_SLOTS - 1)].seq = SPLICE_SEQ_WRITING;
	const int first = SpliceRingDrain(&s_ring, s_out, 64);
	const bool firstOk = first == 0 && s_ring.read == 70;
	const int second = SpliceRingDrain(&s_ring, s_out, 64);
	Check(firstOk && second == 0 && s_ring.read == 71 && s_ring.lost == 7,
	      "ring: an unpublished slot stops the pass, then is abandoned");
	drained += first + second;
	Check(drained + s_ring.lost == s_ring.written && s_ring.read == s_ring.written,
	      "ring: drained plus lost equals written");

	SpliceRingInit(&s_ring);
	PushN(&s_ring, 4, 0);
	s_ring.slot[3].seq = 67;
	Check(SpliceRingDrain(&s_ring, s_out, 64) == 3 && s_ring.lost == 1 && s_ring.overwritten == 1
	      && s_ring.read == 4,
	      "ring: a later lap's record counts overwritten");

	SpliceRingInit(&s_ring);
	s_ring.slot[0].seq = SPLICE_SEQ_WRITING;
	s_ring.written = 64;
	PushN(&s_ring, 1, 64);
	Check(s_ring.claimFailed == 1 && s_ring.slot[0].seq == SPLICE_SEQ_WRITING && s_ring.written == 65,
	      "ring: a held slot fails the claim");

	SpliceRingInit(&s_ring);
	s_ring.slot[1].seq = 65;
	s_ring.written = 1;
	PushN(&s_ring, 1, 1);
	Check(s_ring.claimFailed == 1 && s_ring.slot[1].seq == 65,
	      "ring: a newer record is never overwritten by an older index");
}

int main()
{
	CheckRecord();
	CheckBoxes();
	CheckDecide();
	CheckSite();
	CheckRing();
	return CheckExit("splice_queue_policy_units");
}
