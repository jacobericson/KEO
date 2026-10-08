#include <cstddef>
#include <cstring>
#include <vector>
#include "fixes/world/faction_relations_policy.h"

#include "check.h"

// The hash written out a second time, so a defect in RelKeyHash cannot also
// hide in the table builder.
static unsigned long long TestHash(unsigned long long p)
{
	unsigned long long k = p + (p >> 3);
	k = ~k + (k << 21);
	k = k ^ (k >> 24);
	k = k * 265ULL;
	k = k ^ (k >> 14);
	k = k * 21ULL;
	k = k ^ (k >> 28);
	k = k * 0x80000001ULL;
	return k;
}

// A boost unordered_map node as the engine lays it out: the link, the stored
// hash, then the value (the Faction* key and its RelationData).
struct TNode
{
	uintptr_t          next;
	unsigned long long hash;
	uintptr_t          key;
	unsigned char      head[4];
	float              relation;
	unsigned char      tail[40];
};

// A table with bucketCount + 1 bucket slots, the last one the sentinel whose
// link heads the single list of every node. A bucket holds the address of the
// link before its first node: the sentinel slot's address or a node's.
struct TTable
{
	std::vector<uintptr_t> buckets;
	std::vector<TNode>     nodes;
	RelTable               view;
};

static void TableInit(TTable* t, size_t bucketCount, size_t capacity)
{
	t->buckets.assign(bucketCount + 1, 0);
	t->nodes.clear();
	t->nodes.reserve(capacity);
	t->view.buckets = (uintptr_t)&t->buckets[0];
	t->view.bucketCount = bucketCount;
	t->view.size = 0;
}

// boost 1.60's add_node: a node starting an empty bucket goes to the head of
// the list, after the sentinel; any other goes after its bucket's link.
static TNode* TableInsert(TTable* t, uintptr_t key)
{
	const size_t bc = t->view.bucketCount;
	const unsigned long long mask = (unsigned long long)(bc - 1);
	TNode n;
	memset(&n, 0, sizeof(n));
	n.hash = TestHash(key);
	n.key = key;
	n.relation = 37.5f;
	t->nodes.push_back(n);
	TNode* node = &t->nodes.back();
	const size_t b = (size_t)(node->hash & mask);
	if (t->buckets[b] == 0)
	{
		node->next = t->buckets[bc];
		t->buckets[bc] = (uintptr_t)node;
		if (node->next)
			t->buckets[(size_t)(((TNode*)node->next)->hash & mask)] = (uintptr_t)node;
		t->buckets[b] = (uintptr_t)&t->buckets[bc];
	}
	else
	{
		node->next = *(uintptr_t*)t->buckets[b];
		*(uintptr_t*)t->buckets[b] = (uintptr_t)node;
	}
	++t->view.size;
	return node;
}

static const size_t kFactions = 208;

static uintptr_t FactionKey(size_t i)
{
	return (uintptr_t)(0x000001D4C2A30000ULL + (unsigned long long)i * 0x2A8ULL);
}

static void BuildFactions(TTable* t)
{
	TableInit(t, 256, kFactions);
	for (size_t i = 0; i < kFactions; ++i)
		TableInsert(t, FactionKey(i));
}

static void CheckHash()
{
	Check(RelKeyHash(0x0000000000000000ULL) == 0x77CFA1EEF01BCA90ULL, "hash: known-answer vector 0x0000000000000000");
	Check(RelKeyHash(0x0000000000000001ULL) == 0x5BCA7C69B794F8CEULL, "hash: known-answer vector 0x0000000000000001");
	Check(RelKeyHash(0x0000000000000008ULL) == 0x3A1F6363F43EC697ULL, "hash: known-answer vector 0x0000000000000008");
	Check(RelKeyHash(0x0000000000000010ULL) == 0x743ED0EBE87DA176ULL, "hash: known-answer vector 0x0000000000000010");
	Check(RelKeyHash(0x7FFFFFFFFFFFFFF8ULL) == 0x98ED9027527A9B1FULL, "hash: known-answer vector 0x7FFFFFFFFFFFFFF8");
	Check(RelKeyHash(0xFFFFFFFFFFFFFFF8ULL) == 0xD2AFF580E5F6E0BCULL, "hash: known-answer vector 0xFFFFFFFFFFFFFFF8");
	Check(RelKeyHash(0x000001D4C2A3B000ULL) == 0x52D5BAE2B316BBF4ULL, "hash: known-answer vector 0x000001D4C2A3B000");
	Check(RelKeyHash(0x000001D4C2A3B0A0ULL) == 0x74871568F67972CCULL, "hash: known-answer vector 0x000001D4C2A3B0A0");
	Check(RelKeyHash(0x000002A07F15E340ULL) == 0x9803E9689E4395CEULL, "hash: known-answer vector 0x000002A07F15E340");
	Check(RelKeyHash(0x00007FF6D3A21C80ULL) == 0xB641B9B41A834C05ULL, "hash: known-answer vector 0x00007FF6D3A21C80");

	bool agree = true;
	for (size_t i = 0; i < kFactions; ++i)
		agree = agree && TestHash(FactionKey(i)) == RelKeyHash(FactionKey(i));
	Check(agree, "hash: the test hash agrees with RelKeyHash");
}

static void CheckLayout()
{
	Check(offsetof(TNode, next) == REL_NODE_NEXT && offsetof(TNode, hash) == REL_NODE_HASH
	      && offsetof(TNode, key) == REL_NODE_KEY && offsetof(TNode, relation) == REL_NODE_RELATION,
	      "layout: the test node mirrors the engine's");
}

static void CheckFactionTable()
{
	TTable t;
	BuildFactions(&t);

	size_t count = 0;
	for (uintptr_t n = t.buckets[256]; n && count <= kFactions; n = ((TNode*)n)->next)
		++count;
	Check(count == kFactions, "builder: every key is reachable from the sentinel bucket");

	bool same = true, holds = true;
	for (size_t i = 0; i < kFactions; ++i)
	{
		uintptr_t node = 1, walk = 1;
		RelFindResult a = RelLookup(t.view, FactionKey(i), &node);
		RelFindResult b = RelWalkFind(t.view, FactionKey(i), &walk);
		same = same && a == REL_FOUND && b == REL_FOUND && node == walk && node != 0;
		holds = holds && node != 0 && node != 1 && *(uintptr_t*)(node + REL_NODE_KEY) == FactionKey(i);
	}
	Check(same, "lookup: every key of a 208-entry table is found where the walk finds it");
	Check(holds, "lookup: the node found holds the key");

	// Absent keys: one in an empty bucket, one in a non-empty bucket, and one more.
	uintptr_t inEmpty = 0, inFull = 0;
	for (size_t i = kFactions; i < kFactions + 4096 && (!inEmpty || !inFull); ++i)
	{
		uintptr_t k = FactionKey(i);
		size_t b = (size_t)(TestHash(k) & 255);
		if (t.buckets[b] == 0 && !inEmpty)
			inEmpty = k;
		else if (t.buckets[b] != 0 && !inFull)
			inFull = k;
	}
	const uintptr_t absent[3] = { inEmpty, inFull, (uintptr_t)0x00000000DEADBEE0ULL };
	bool allAbsent = inEmpty != 0 && inFull != 0;
	for (int i = 0; i < 3; ++i)
	{
		uintptr_t node = 1, walk = 1;
		allAbsent = allAbsent && RelLookup(t.view, absent[i], &node) == REL_ABSENT && node == 0
		                      && RelWalkFind(t.view, absent[i], &walk) == REL_ABSENT && walk == 0;
	}
	Check(allAbsent, "lookup: a key not in the table is absent");
}

static void CheckSharedBucket()
{
	uintptr_t first = 0, second = 0;
	for (unsigned i = 0; i < 4096 && !second; ++i)
	{
		uintptr_t a = (uintptr_t)(0x00007FF600000000ULL + (unsigned long long)i * 0x10ULL);
		for (unsigned j = i + 1; j < 4096; ++j)
		{
			uintptr_t b = (uintptr_t)(0x00007FF600000000ULL + (unsigned long long)j * 0x10ULL);
			if ((TestHash(a) & 255) == (TestHash(b) & 255))
			{
				first = a;
				second = b;
				break;
			}
		}
	}
	TTable t;
	TableInit(&t, 256, 2);
	bool ok = second != 0;
	if (ok)
	{
		TNode* na = TableInsert(&t, first);
		TNode* nb = TableInsert(&t, second);
		uintptr_t node = 0;
		ok = RelLookup(t.view, first, &node) == REL_FOUND && node == (uintptr_t)na;
		ok = ok && RelLookup(t.view, second, &node) == REL_FOUND && node == (uintptr_t)nb;
	}
	Check(ok, "lookup: two keys sharing a bucket are both found");
}

static void CheckRefusals()
{
	{
		RelTable empty;
		empty.buckets = 0;
		empty.bucketCount = 256;
		empty.size = 0;
		uintptr_t node = 1, walk = 1;
		Check(RelLookup(empty, FactionKey(0), &node) == REL_ABSENT && node == 0
		      && RelWalkFind(empty, FactionKey(0), &walk) == REL_ABSENT && walk == 0,
		      "lookup: an empty table is absent without reading a bucket");
	}

	uintptr_t slots[4] = { 0, 0, 0, 0 };
	RelTable odd;
	odd.buckets = (uintptr_t)&slots[0];
	odd.size = 1;
	uintptr_t node = 1;
	odd.bucketCount = 0;
	Check(RelLookup(odd, FactionKey(0), &node) == REL_LIMIT, "lookup: a bucket count of 0 is refused");
	odd.bucketCount = 3;
	Check(RelLookup(odd, FactionKey(0), &node) == REL_LIMIT,
	      "lookup: a bucket count that is not a power of two is refused");

	// One node linked to itself: its bucket's chain never ends.
	TTable t;
	TableInit(&t, 1, 1);
	TNode* n = TableInsert(&t, FactionKey(0));
	const uintptr_t probe = FactionKey(1);
	n->hash = TestHash(probe);
	n->next = (uintptr_t)n;
	Check(RelLookup(t.view, probe, &node) == REL_LIMIT, "lookup: a chain longer than the table is refused");
	Check(RelWalkFind(t.view, probe, &node) == REL_LIMIT, "walk: a cycle is refused");
}

static void CheckVerify()
{
	const uintptr_t a = 0x1000, b = 0x2000;
	const unsigned long long h = 0x1234ULL;
	Check(RelVerifyLookup(a, a, h, h) == REL_VERIFY_OK, "verify: the same node is a match");
	Check(RelVerifyLookup(0, 0, 0, h) == REL_VERIFY_OK, "verify: both absent is a match");
	Check(RelVerifyLookup(0, b, h, h) == REL_VERIFY_NODE, "verify: a lookup that misses the walk's node is a mismatch");
	Check(RelVerifyLookup(0, b, h + 1, h) == REL_VERIFY_HASH,
	      "verify: a walk node whose stored hash differs is a hash mismatch");
}

static void CheckWrite()
{
	TNode n;
	memset(&n, 0xA5, sizeof(n));
	n.relation = 37.5f;
	TNode before = n;
	RelWriteSelf((uintptr_t)&n);
	Check(n.relation == 100.0f, "write: the self entry reads 100");
	const unsigned char* p = (const unsigned char*)&n;
	const unsigned char* q = (const unsigned char*)&before;
	Check(memcmp(p, q, REL_NODE_RELATION) == 0
	      && memcmp(p + REL_NODE_RELATION + 4, q + REL_NODE_RELATION + 4, sizeof(TNode) - REL_NODE_RELATION - 4) == 0,
	      "write: nothing else in the node changes");
}

static void CheckWindow()
{
	Check(RelArmVerifyWindow(REL_MODE_OFF, REL_MODE_ON) && RelArmVerifyWindow(REL_MODE_VERIFY, REL_MODE_ON),
	      "window: switching to on arms the verify window");
	Check(!RelArmVerifyWindow(REL_MODE_ON, REL_MODE_ON), "window: on to on does not re-arm");
	Check(!RelArmVerifyWindow(REL_MODE_ON, REL_MODE_OFF) && !RelArmVerifyWindow(REL_MODE_OFF, REL_MODE_VERIFY)
	      && !RelArmVerifyWindow(REL_MODE_ON, REL_MODE_VERIFY) && !RelArmVerifyWindow(REL_MODE_VERIFY, REL_MODE_OFF),
	      "window: switching to off or verify does not arm");
}

static void CheckPath()
{
	const uintptr_t me = 0x5000;
	bool off = true;
	for (int f = 0; f < 2; ++f)
		for (int w = 0; w < 2; ++w)
		{
			off = off && RelChoosePath(REL_MODE_OFF, f != 0, me, w != 0) == REL_PATH_OFF;
			off = off && RelChoosePath(REL_MODE_OFF, f != 0, 0, w != 0) == REL_PATH_OFF;
		}
	Check(off, "path: off forwards without counting");
	Check(RelChoosePath(REL_MODE_ON, true, me, false) == REL_PATH_FORWARD
	      && RelChoosePath(REL_MODE_ON, true, me, true) == REL_PATH_FORWARD
	      && RelChoosePath(REL_MODE_VERIFY, true, me, false) == REL_PATH_FORWARD,
	      "path: fallback forwards");
	Check(RelChoosePath(REL_MODE_ON, false, 0, false) == REL_PATH_FORWARD
	      && RelChoosePath(REL_MODE_VERIFY, false, 0, false) == REL_PATH_FORWARD,
	      "path: a NULL faction forwards");
	Check(RelChoosePath(REL_MODE_VERIFY, false, me, false) == REL_PATH_VERIFY, "path: verify mode verifies");
	Check(RelChoosePath(REL_MODE_ON, false, me, true) == REL_PATH_VERIFY, "path: on inside the window verifies");
	Check(RelChoosePath(REL_MODE_ON, false, me, false) == REL_PATH_LOOKUP, "path: on after the window looks up");
}

int main()
{
	CheckHash();
	CheckLayout();
	CheckFactionTable();
	CheckSharedBucket();
	CheckRefusals();
	CheckVerify();
	CheckWrite();
	CheckWindow();
	CheckPath();
	return CheckExit("faction_relations_units");
}
