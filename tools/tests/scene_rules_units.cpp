// Host tests for the scene and render probes' pure rules (audit_scene_rules.h).

#include "../../profiler/audit_scene_rules.h"

#include <string.h>

using namespace scenerules;

#include "check.h"

static void Levels()
{
	Check(LevelBin(0) == LB_EMPTY, "a level with no node is empty");
	Check(LevelBin(1) == LB_SMALL && LevelBin(64) == LB_SMALL, "64 nodes is small");
	Check(LevelBin(65) == LB_MID && LevelBin(512) == LB_MID, "65 to 512 nodes is mid");
	Check(LevelBin(513) == LB_LARGE, "513 nodes is large");
}

static void Classes()
{
	const size_t ent = 0x10C1080, hw = 0x112FBA0, vtf = 0x1130E90, dflt = 0x10274C0, thunk = 0x7000;
	Check(VisClass(ent, dflt, ent, hw, vtf) == VC_ENTITY, "an entity is an entity");
	Check(VisClass(0x5000, hw, ent, hw, vtf) == VC_BATCH && VisClass(0x5000, vtf, ent, hw, vtf) == VC_BATCH,
	      "both batch overrides are batches");
	Check(VisClass(0x5000, thunk, ent, hw, vtf) == VC_OTHER, "a foreign class's thunk in the cull slot is not a batch");
	Check(VisClass(0x5000, dflt, ent, hw, vtf) == VC_OTHER, "the default cull slot is other");
}

static void Queues()
{
	unsigned char blocks[6 * 160];
	memset(blocks, 0, sizeof(blocks));
	const size_t counts[6] = { 3, 0, 0, 5, 0, 2 };
	for (int i = 0; i < 6; ++i)
		memcpy(blocks + i * 160 + 64, &counts[i], sizeof(size_t));
	QueueCount q = { 0, 0, 0 };
	CountQueues(blocks, 160, 64, 1, 5, 4, &q);
	Check(q.queues == 3, "the walk ends at the smaller of the queue count and the last queue");
	Check(q.empty == 2 && q.objects == 5, "empty queues and objects are counted");
	QueueCount z = { 0, 0, 0 };
	CountQueues(blocks, 160, 64, 4, 2, 6, &z);
	Check(z.queues == 0, "an empty range walks nothing");
}

static void Skips()
{
	Check(WouldSkip(true, true, true, true, true), "a flagged state with an equal desc and reference is a skip");
	Check(!WouldSkip(true, true, false, true, true), "nothing bound is never a skip");
	Check(!WouldSkip(true, false, true, true, true), "no shadow (after ClearState) is never a skip");
	Check(!WouldSkip(true, true, true, false, true), "a changed desc is not a skip");
	Check(!WouldSkip(true, true, true, true, false), "a changed stencil reference is not a skip");
	Check(!WouldSkip(false, true, true, true, true), "an unflagged state is not counted");
}

static void EmptyDraws()
{
	unsigned long long vd[7];
	memset(vd, 0, sizeof(vd));
	const void* op[1] = { NULL };
	Check(DrawsNothing(op), "an operation with no vertex data draws nothing");
	op[0] = vd;
	Check(DrawsNothing(op), "an operation with no vertices draws nothing");
	vd[6] = 4;   // the vertex count at +0x30
	Check(!DrawsNothing(op), "an operation with vertices reaches the state section");
}

static void Consumed()
{
	Check(FlagConsumed(0) && !FlagConsumed(1), "a change counts only when the draw consumed the flag");
	Check(TakesShadow(0, true), "a consumed flag with an object bound takes the shadow");
	Check(!TakesShadow(1, true), "a flag the draw left set takes no shadow");
	Check(!TakesShadow(0, false), "nothing bound takes no shadow");
}

static void Slots()
{
	Check(SlotKind(0x20, 0x10, 0) == SLOT_UNCLASSED && SlotKind(0x10, 0, 0x20) == SLOT_UNCLASSED,
	      "one main semaphore published classes nothing");
	Check(SlotKind(0x10, 0, 0) == SLOT_UNCLASSED, "no main semaphore published classes nothing");
	Check(SlotKind(0x10, 0x10, 0x20) == SLOT_MAIN && SlotKind(0x20, 0x10, 0x20) == SLOT_MAIN,
	      "either main semaphore is a main worker");
	Check(SlotKind(0x30, 0x10, 0x20) == SLOT_OTHER, "another semaphore is another scene manager's");
}

struct FakeNode { unsigned char b[0x30]; };
struct FakeList { const void* first; const void* last; };

static void Put(FakeNode& n, size_t off, const void* p)
{
	memcpy(n.b + off, &p, sizeof(p));
}

static void Node(FakeNode& n, FakeNode* left, FakeNode* parent, FakeNode* right, const FakeList* value, bool nil)
{
	memset(n.b, 0, sizeof(n.b));
	Put(n, NODE_LEFT, left);
	Put(n, NODE_PARENT, parent);
	Put(n, NODE_RIGHT, right);
	Put(n, NODE_VALUE, value);
	n.b[NODE_ISNIL] = nil ? 1 : 0;
}

static void Map()
{
	// B is the root; A < B < C < D < E with D = {C, E}; leaves link to the head H.
	static FakeNode H, A, B, C, D, E;
	static char buf[4];
	static FakeList full = { buf, buf + 1 }, none = { buf, buf };
	Node(H, &A, &B, &E, NULL, true);
	Node(A, &H, &B, &H, &full, false);
	Node(B, &A, &H, &D, &none, false);
	Node(C, &H, &D, &H, &full, false);
	Node(D, &C, &B, &E, &none, false);
	Node(E, &H, &D, &H, &full, false);
	const unsigned char* a = A.b;
	Check(MapNext(a) == B.b && MapNext(B.b) == C.b && MapNext(C.b) == D.b && MapNext(D.b) == E.b && MapNext(E.b) == H.b,
	      "the in-order walk visits every node once");
	size_t nonEmpty = 0;
	Check(MapWalk(H.b, 1000, &nonEmpty) == 5 && nonEmpty == 3, "the walk counts the nodes and the non-empty lists");
	Check(MapWalk(H.b, 2, &nonEmpty) == 2, "the walk stops at its bound");
	static FakeNode H2;
	Node(H2, &H2, &H2, &H2, NULL, true);
	Check(MapWalk(H2.b, 1000, &nonEmpty) == 0 && nonEmpty == 0, "an empty map walks nothing");
}

int main()
{
	Levels();
	Classes();
	Queues();
	Skips();
	EmptyDraws();
	Consumed();
	Slots();
	Map();
	return CheckExit("scene_rules_units");
}
