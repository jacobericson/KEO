// The NPC failed-search memo's pure rules: which searches it covers, the key, the bounded table and
// its staleness, and the face and fingerprint reads over a collection built in memory in the
// engine's layout.

#include "pathfind/npc_fail_memo_policy.h"
#include <string.h>

#include "check.h"

static const char* const SUITE_NAME = "npc_fail_memo_units";
static const long long TPS = 1000;   // ticks per second in these cases

// Two section slots: slot 0 holds an instance with two original faces (face 1 mapped to the
// instanced copy 0) and one owned face; slot 1 is empty.
struct __declspec(align(16)) FakeWorld
{
	unsigned char coll[0x38];
	unsigned char infos[2 * NFM_INFO_STRIDE];
	unsigned char inst[0x1C0];
	unsigned char origFaces[2 * NFM_FACE_BYTES];
	unsigned char instFaces[1 * NFM_FACE_BYTES];
	unsigned char ownedFaces[1 * NFM_FACE_BYTES];
	int faceMap[2];
	int origData[2];
	int instData[1];
	int ownedData[1];
};

static void PutPtr(unsigned char* base, size_t off, const void* p) { memcpy(base + off, &p, sizeof(p)); }
static void PutInt(unsigned char* base, size_t off, int v) { memcpy(base + off, &v, sizeof(v)); }
static void PutCluster(unsigned char* face, short c) { memcpy(face + NFM_FACE_CLUSTER, &c, sizeof(c)); }

static void Build(FakeWorld* w)
{
	memset(w, 0, sizeof(*w));
	PutPtr(w->coll, NFM_COLL_INSTANCES, w->infos);
	PutInt(w->coll, NFM_COLL_COUNT, 2);
	PutPtr(w->infos, 0, w->inst);
	PutPtr(w->inst, NFM_INST_ORIG_FACES, w->origFaces);
	PutInt(w->inst, NFM_INST_NUM_ORIG, 2);
	PutPtr(w->inst, NFM_INST_ORIG_DATA, w->origData);
	PutInt(w->inst, NFM_INST_DATA_STRIDE, 1);
	w->faceMap[0] = -1;
	w->faceMap[1] = 0;
	PutPtr(w->inst, NFM_INST_FACE_MAP, w->faceMap);
	PutInt(w->inst, NFM_INST_FACE_MAP + 8, 2);
	PutPtr(w->inst, NFM_INST_INST_FACES, w->instFaces);
	PutInt(w->inst, NFM_INST_INST_FACES + 8, 1);
	PutPtr(w->inst, NFM_INST_OWNED_FACES, w->ownedFaces);
	PutInt(w->inst, NFM_INST_OWNED_FACES + 8, 1);
	PutPtr(w->inst, NFM_INST_INST_DATA, w->instData);
	PutPtr(w->inst, NFM_INST_OWNED_DATA, w->ownedData);
	PutInt(w->inst, NFM_INST_RUNTIME_ID, 7);
	PutCluster(w->origFaces, 11);
	PutCluster(w->origFaces + NFM_FACE_BYTES, 99);
	PutCluster(w->instFaces, 12);
	PutCluster(w->ownedFaces, -1);
	w->origData[0] = 4;
	w->origData[1] = 9;
	w->instData[0] = 5;
	w->ownedData[0] = 3;
}

static void CheckCovers()
{
	Check(NfmCovers(NFM_ON, ASTAR_CALLER_CHARACTER_NPC, false, false)
	      && NfmCovers(NFM_OBSERVE, ASTAR_CALLER_CHARACTER_NPC, false, false),
	      "covers: an NPC's serve-path search under observe and on");
	Check(!NfmCovers(NFM_OFF, ASTAR_CALLER_CHARACTER_NPC, false, false), "covers: nothing while off");
	const AstarCallerClass others[] =
	{
		ASTAR_CALLER_CHARACTER_PLAYER, ASTAR_CALLER_CHARACTER_UNKNOWN, ASTAR_CALLER_GATE,
		ASTAR_CALLER_GENERATION, ASTAR_CALLER_HAVOK_INTERNAL, ASTAR_CALLER_OTHER
	};
	bool none = true;
	for (int i = 0; i < 6; ++i)
		none = none && !NfmCovers(NFM_ON, others[i], false, false);
	Check(none, "covers: never a player, unattributed, gate, generation or Havok search");
	Check(!NfmCovers(NFM_ON, ASTAR_CALLER_CHARACTER_NPC, true, false), "covers: never a search carrying the player tag");
	Check(!NfmCovers(NFM_ON, ASTAR_CALLER_CHARACTER_NPC, false, true), "covers: never a search waved past the cluster graph");
}

static void CheckKey()
{
	unsigned start = (5u << 22) | 1234u;
	unsigned other = (5u << 22) | 1300u;
	unsigned goal = (2u << 22) | 77u;
	Check(NfmClusterKey(start, 37) == ((5u << 22) | 37u), "key: the cluster key is the cluster index with the face's section");
	Check(NfmClusterKey(start, -1) == NFM_NO_KEY, "key: a face with no cluster has no cluster key");
	NfmKey a = NfmMakeKey(goal, start, 37, 0x40000000u, false);
	Check(a.start == ((5u << 22) | 37u) && a.flags == 0u && a.goal == goal, "key: a start with a cluster keys by its cluster");
	NfmKey f = NfmMakeKey(goal, start, -1, 0x40000000u, false);
	Check(f.start == start && f.flags == NFM_START_FACE, "key: a start with no cluster keys by its face");
	Check((NfmMakeKey(goal, start, 37, 0x40000000u, true).flags & NFM_COST_MODIFIER) != 0
	      && !NfmKeyEqual(a, NfmMakeKey(goal, start, 37, 0x40000000u, true)), "key: a cost modifier is part of the key");
	Check(NfmKeyEqual(a, NfmMakeKey(goal, other, 37, 0x40000000u, false)), "key: two faces of one cluster share a key");
	Check(!NfmKeyEqual(a, NfmMakeKey(goal, start, 37, 0x3F800000u, false)), "key: another agent diameter is another key");
	NfmKey byFace = NfmMakeKey(goal, (5u << 22) | 37u, -1, 0x40000000u, false);
	Check(!NfmKeyEqual(a, byFace), "key: a face key never equals a cluster key of the same number");
}

static NfmEpoch Epoch(unsigned sections, long door, long reset)
{
	NfmEpoch e;
	e.sections = sections;
	e.door = door;
	e.reset = reset;
	return e;
}

static NfmTable s_table;

static void CheckTable()
{
	long long ttl = NfmTtlTicks(TPS, true);
	NfmEpoch e0 = Epoch(1u, 0, 0);
	NfmKey k = NfmMakeKey(100u, 200u, 3, 0x40000000u, false);
	NfmKey other = NfmMakeKey(101u, 200u, 3, 0x40000000u, false);

	NfmClear(&s_table);
	NfmInsert(&s_table, k, 200u, e0, 100, 50, 3, 3);
	NfmLookup l = NfmFind(&s_table, k, 200u, e0, 200, ttl);
	Check(l.index >= 0 && l.exactFace == 1 && s_table.e[l.index].serviceTicks == 50, "table: a failure inserted is found");
	l = NfmFind(&s_table, k, 201u, e0, 200, ttl);
	Check(l.index >= 0 && l.exactFace == 0, "table: a cluster match on another face is not exact");
	Check(NfmFind(&s_table, other, 200u, e0, 200, ttl).index < 0, "table: another goal misses");

	l = NfmFind(&s_table, k, 200u, Epoch(2u, 0, 0), 200, ttl);
	Check(l.index < 0 && l.dropped[NFM_DROP_SECTIONS] == 1 && s_table.entries == 0, "table: a navmesh change retires the entry");
	NfmInsert(&s_table, k, 200u, e0, 100, 50, 3, 3);
	l = NfmFind(&s_table, k, 200u, Epoch(1u, 1, 0), 200, ttl);
	Check(l.index < 0 && l.dropped[NFM_DROP_DOOR] == 1, "table: a door change retires the entry");
	NfmInsert(&s_table, k, 200u, e0, 100, 50, 3, 3);
	l = NfmFind(&s_table, k, 200u, Epoch(1u, 0, 1), 200, ttl);
	Check(l.index < 0 && l.dropped[NFM_DROP_RESET] == 1, "table: a new reset generation retires the entry");
	NfmInsert(&s_table, k, 200u, e0, 100, 50, 3, 3);
	Check(NfmFind(&s_table, k, 200u, e0, 100 + ttl - 1, ttl).index >= 0, "table: an entry younger than the time-to-live answers");
	l = NfmFind(&s_table, k, 200u, e0, 100 + ttl, ttl);
	Check(l.index < 0 && l.dropped[NFM_DROP_TTL] == 1, "table: an entry 15 s old retires");

	NfmClear(&s_table);
	NfmInsert(&s_table, k, 200u, e0, 100, 50, 3, 3);
	bool fresh = NfmInsert(&s_table, k, 200u, e0, 300, 60, 3, 3);
	l = NfmFind(&s_table, k, 200u, e0, 310, ttl);
	Check(!fresh && s_table.entries == 1 && l.index >= 0 && s_table.e[l.index].insertTicks == 300,
	      "table: a second failure refreshes the key's slot");
	Check(NfmErase(&s_table, k) && s_table.entries == 0 && NfmFind(&s_table, k, 200u, e0, 310, ttl).index < 0,
	      "table: observe's wrong answer erases the entry");

	NfmClear(&s_table);
	NfmKey last = k;
	for (unsigned i = 0; i < 1000; ++i)
	{
		last = NfmMakeKey(1000u + i, 7u, 1, 0x40000000u, false);
		NfmInsert(&s_table, last, 7u, e0, (long long)i, 1, 3, 3);
	}
	int used = 0;
	for (int i = 0; i < NFM_TABLE_SIZE; ++i)
		used += s_table.e[i].used ? 1 : 0;
	Check(s_table.entries <= NFM_TABLE_SIZE && used == s_table.entries, "table: the table never holds more than its size");
	Check(NfmFind(&s_table, last, 7u, e0, 1000, ttl).index >= 0, "table: the newest failure is always kept");

	Check(NfmShouldInsert(3, 3, 20000) && !NfmShouldInsert(3, 3, 19999) && !NfmShouldInsert(3, 1, 50000)
	      , "insert: a node-cap failure needs its cause and at least 20000 iterations");
	Check(NfmShouldInsert(2, 0, 20000), "memo: an exhaustive unreachable search past the threshold is inserted");
	Check(!NfmShouldInsert(2, 0, 19999), "memo: a cheap unreachable search is not inserted");
	Check(!NfmShouldInsert(0, 0, 50000) && !NfmShouldInsert(1, 0, 50000)
	      && !NfmShouldInsert(4, 0, 50000) && !NfmShouldInsert(5, 0, 50000),
	      "memo: incomplete, successful, truncated and invalid outputs never insert");
	Check(NfmWrong(1) && !NfmWrong(2) && !NfmWrong(3), "observe: only a success is wrong");
	Check(NfmTtlTicks(TPS, true) == 15 * TPS && NfmTtlTicks(TPS, false) == 5 * TPS, "ttl: 15 s with door changes seen, 5 s without");
}


static void CheckReplay()
{
	NfmClear(&s_table);
	NfmKey k = NfmMakeKey(100u, 200u, 3, 0x40000000u, false);
	NfmEpoch epoch = Epoch(1u, 0, 0);
	NfmInsert(&s_table, k, 200u, epoch, 100, 50, 2, 0);
	NfmLookup look = NfmFind(&s_table, k, 200u, epoch, 101, 15000);
	unsigned char output[0x40];
	memset(output, 0xA5, sizeof(output));
	NfmReplay(s_table.e[look.index], output);
	Check(output[NFM_OUT_STATUS] == 2 && output[NFM_OUT_CAUSE] == 0,
	      "memo: an entry replays the status and cause it recorded");
	bool untouched = true;
	for (size_t i = 0; i < sizeof(output); ++i)
		if (i != NFM_OUT_STATUS && i != NFM_OUT_CAUSE)
			untouched = untouched && output[i] == 0xA5;
	Check(untouched, "memo: replay changes no other output field");
	NfmInsert(&s_table, k, 200u, epoch, 102, 60, 3, 3);
	NfmReplay(s_table.e[look.index], output);
	Check(output[NFM_OUT_STATUS] == 3 && output[NFM_OUT_CAUSE] == 3 && s_table.entries == 1,
	      "memo: a refreshed entry takes the new search's status");
	NfmInsert(&s_table, k, 200u, epoch, 103, 70, 2, 0);
	NfmReplay(s_table.e[look.index], output);
	Check(output[NFM_OUT_STATUS] == 2 && output[NFM_OUT_CAUSE] == 0,
	      "memo: a node-cap entry refreshes to unreachable too");
	Check(NfmWrongClass(2, 3) && NfmWrongClass(3, 2) && !NfmWrongClass(2, 2)
	      && !NfmWrongClass(3, 3) && !NfmWrongClass(2, 1),
	      "observe: only a different failed status is wrongClass");
}

static void CheckInputCoverage()
{
	__declspec(align(16)) unsigned char input[0xA0];
	memset(input, 0, sizeof(input));
	PutInt(input, NFM_IN_MAX_LENGTH, 0x7F7FFFEE);
	PutInt(input, NFM_IN_SPHERE, (int)0xBF800000u);
	PutInt(input, NFM_IN_CAPSULE, (int)0xBF800000u);
	Check(NfmUnreachableInputCovered(input), "memo: the unbounded character input covers unreachable");
	PutInt(input, NFM_IN_SPHERE, 0);
	Check(!NfmUnreachableInputCovered(input), "memo: a search sphere excludes unreachable");
	PutInt(input, NFM_IN_SPHERE, (int)0xBF800000u);
	PutInt(input, NFM_IN_CAPSULE, 0x3F800000);
	Check(!NfmUnreachableInputCovered(input), "memo: a search capsule excludes unreachable");
	PutInt(input, NFM_IN_CAPSULE, (int)0xBF800000u);
	PutInt(input, NFM_IN_MAX_LENGTH, 0x447A0000);
	Check(!NfmUnreachableInputCovered(input), "memo: a maximum path cost excludes unreachable");
	Check(!NfmUnreachableInputCovered(NULL), "memo: missing input excludes unreachable");
	NfmKey a = NfmMakeKey(100u, 200u, 3, 0x40000000u, true, 0x40000000u);
	NfmKey b = NfmMakeKey(100u, 200u, 3, 0x40000000u, true, 0x40400000u);
	Check(!NfmKeyEqual(a, b), "key: different water costs cannot alias despite sharing the modifier flag");
	Check(NfmKeyEqual(NfmMakeKey(100u, 200u, 3, 0x40000000u, false, 1u),
	                  NfmMakeKey(100u, 200u, 3, 0x40000000u, false, 2u)),
	      "key: absent modifiers ignore unused scalar bits");
}

static void CheckFaces()
{
	FakeWorld w;
	Build(&w);
	int cluster = 0, data = 0;
	Check(NfmReadFace(w.coll, 0u, &cluster, &data) && cluster == 11 && data == 4, "faces: an original face reads its cluster and data word");
	Check(NfmReadFace(w.coll, 1u, &cluster, &data) && cluster == 12 && data == 5, "faces: a mapped face reads the instanced copy");
	Check(NfmReadFace(w.coll, 2u, &cluster, &data) && cluster == -1 && data == 3, "faces: an owned face reads past the original count");
	Check(!NfmReadFace(w.coll, 3u, &cluster, &data) && cluster == -1 && data == -1, "faces: an index past the owned faces is refused");
	Check(!NfmReadFace(w.coll, 1u << 22, &cluster, &data), "faces: an empty section slot is refused");
	Check(!NfmReadFace(w.coll, 5u << 22, &cluster, &data), "faces: a section past the collection is refused");
	Check(!NfmReadFace(w.coll, NFM_NO_KEY, &cluster, &data), "faces: no key reads nothing");
	PutInt(w.inst, NFM_INST_DATA_STRIDE, 0);
	Check(NfmReadFace(w.coll, 0u, &cluster, &data) && cluster == 11 && data == -1, "faces: no face data reads -1");
	Build(&w);
	PutInt(w.inst, NFM_INST_FACE_MAP + 8, 0);
	Check(NfmReadFace(w.coll, 0u, &cluster, &data) && cluster == 12 && data == 5,
	      "faces: without a face map the face index indexes the instanced faces, as the engine does");
}

static void CheckFingerprint()
{
	FakeWorld w;
	Build(&w);
	bool ok1 = false, ok2 = false;
	unsigned a = NfmSectionsFingerprint(w.coll, &ok1);
	unsigned b = NfmSectionsFingerprint(w.coll, &ok2);
	Check(ok1 && ok2 && a == b, "fingerprint: an unchanged collection reads the same");
	PutInt(w.inst, NFM_INST_RUNTIME_ID, 8);
	Check(NfmSectionsFingerprint(w.coll, &ok1) != a && ok1, "fingerprint: a re-registered instance changes it");
	Build(&w);
	PutPtr(w.inst, NFM_INST_ORIG_FACES, w.instFaces);
	Check(NfmSectionsFingerprint(w.coll, &ok1) != a, "fingerprint: a regenerated mesh changes it");
	Build(&w);
	PutPtr(w.infos + NFM_INFO_STRIDE, 0, w.inst);
	Check(NfmSectionsFingerprint(w.coll, &ok1) != a, "fingerprint: an added section changes it");
	Build(&w);
	PutInt(w.coll, NFM_COLL_COUNT, NFM_MAX_SECTIONS + 1);
	NfmSectionsFingerprint(w.coll, &ok1);
	Check(!ok1, "fingerprint: a count past the bound is refused");
}

int main()
{
	CheckCovers();
	CheckKey();
	CheckTable();
	CheckReplay();
	CheckInputCoverage();
	CheckFaces();
	CheckFingerprint();
	return CheckExit(SUITE_NAME);
}
