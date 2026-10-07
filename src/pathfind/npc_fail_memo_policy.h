#ifndef KEO_NPC_FAIL_MEMO_POLICY_H
#define KEO_NPC_FAIL_MEMO_POLICY_H

// The NPC failed-search memo's pure rules: which searches it covers, the key, the bounded table and
// when an entry stops answering, and the reads of a packed face key through the streaming
// collection's memory layout. No game or Windows header, so the host suite links it alone.

#include "pathfind/astar_cost_policy.h"
#include <stddef.h>

enum NfmMode { NFM_OFF = 0, NFM_OBSERVE = 1, NFM_ON = 2 };

const int      NFM_TABLE_SIZE          = 256;     // a power of two
const int      NFM_PROBE               = 4;       // slots tried from a key's home slot
const int      NFM_MIN_INSERT_ITER     = 20000;   // a cheaper failure is not worth an entry
const int      NFM_TTL_SECONDS         = 15;
const int      NFM_TTL_NO_DOOR_SECONDS = 5;       // while door changes are not seen
const unsigned NFM_NO_KEY              = 0xFFFFFFFFu;
const int      NFM_MAX_SECTIONS        = 4096;    // a larger slot count is not a collection

// Key flags.
const unsigned NFM_START_FACE    = 1u;   // the start is the exact face key: its face has no cluster
const unsigned NFM_COST_MODIFIER = 2u;   // the search carried a cost modifier

// FindPathInput and FindPathOutput fields the memo reads and writes.
const size_t NFM_IN_START_FACE  = 0x30;
const size_t NFM_IN_GOAL_KEYS   = 0x38;   // hkArray<unsigned>: the data, then the int count at +8
const size_t NFM_IN_AGENT_DIAM  = 0x4C;
const size_t NFM_IN_COST_MOD    = 0x70;
const size_t NFM_IN_MAX_LENGTH  = 0x90;
const size_t NFM_IN_SPHERE      = 0x94;
const size_t NFM_IN_CAPSULE     = 0x98;
const size_t NFM_MOD_WATER_COST = 0x20;   // WaterCostModifier scalar, replicated across its vector
const size_t NFM_OUT_STATUS     = 0x3C;
const size_t NFM_OUT_CAUSE      = 0x3D;

// The streaming collection and navmesh instance layout the engine's own face lookups read.
const size_t NFM_COLL_INSTANCES   = 0x20;   // 48-byte records, the instance pointer first
const size_t NFM_COLL_COUNT       = 0x28;
const size_t NFM_INFO_STRIDE      = 48;
const size_t NFM_INST_ORIG_FACES  = 0x10;
const size_t NFM_INST_NUM_ORIG    = 0x18;
const size_t NFM_INST_ORIG_DATA   = 0x40;
const size_t NFM_INST_DATA_STRIDE = 0x48;
const size_t NFM_INST_FACE_MAP    = 0xE0;   // each array here: the data, then the int size at +8
const size_t NFM_INST_INST_FACES  = 0xF0;
const size_t NFM_INST_OWNED_FACES = 0x110;
const size_t NFM_INST_INST_DATA   = 0x160;
const size_t NFM_INST_OWNED_DATA  = 0x180;
const size_t NFM_INST_RUNTIME_ID  = 0x1A4;
const size_t NFM_FACE_BYTES       = 16;
const size_t NFM_FACE_CLUSTER     = 12;     // int16

// Covered: an NPC's own path request, served at the character call site at NPC priority, with no
// player tag on the thread, not waved past the cluster graph, while the mode is not off.
bool NfmCovers(int mode, AstarCallerClass cls, bool playerTag, bool waved);

struct NfmKey
{
	unsigned goal;      // the goal face key
	unsigned start;     // the start's cluster key, or its face key with NFM_START_FACE
	unsigned diameter;  // the agent diameter's bits
	unsigned flags;
	unsigned waterCost; // modifier scalar bits, zero without a modifier
};

// The engine's cluster key for a face: the cluster index with the face's section in the high ten
// bits. NFM_NO_KEY when the face has no cluster.
unsigned NfmClusterKey(unsigned faceKey, int cluster);
NfmKey   NfmMakeKey(unsigned goalFace, unsigned startFace, int startCluster, unsigned diameterBits, bool costModifier,
                    unsigned waterCostBits = 0);
bool     NfmKeyEqual(const NfmKey& a, const NfmKey& b);
unsigned NfmHash(const NfmKey& k);

// What an entry was valid for when it was made.
struct NfmEpoch
{
	unsigned sections;   // NfmSectionsFingerprint
	long     door;       // door-state changes seen
	long     reset;      // mode changes and save loads
};

struct NfmEntry
{
	NfmKey    key;
	unsigned  startFace;      // the inserting search's own start face
	NfmEpoch  epoch;
	long long insertTicks;
	long long serviceTicks;   // what the inserting search cost
	int       status;
	int       cause;
	int       used;
};

struct NfmTable
{
	NfmEntry e[NFM_TABLE_SIZE];
	int      entries;         // used slots
};

enum NfmDrop { NFM_LIVE = 0, NFM_DROP_RESET, NFM_DROP_SECTIONS, NFM_DROP_DOOR, NFM_DROP_TTL, NFM_DROP_COUNT };

// Why an entry no longer answers, in this order: reset, sections, door, age.
NfmDrop NfmStale(const NfmEntry& e, const NfmEpoch& now, long long nowTicks, long long ttlTicks);

struct NfmLookup
{
	int index;                     // the live entry, -1 when none
	int exactFace;                 // its start face is the caller's own
	int dropped[NFM_DROP_COUNT];   // stale entries met in the probe window and cleared, by reason
};

NfmLookup NfmFind(NfmTable* t, const NfmKey& k, unsigned startFace, const NfmEpoch& now,
                  long long nowTicks, long long ttlTicks);
// Records a failure: refreshes the key's slot, else takes the first free slot of the probe window,
// else the one inserted longest ago. True when it took a free slot.
bool NfmInsert(NfmTable* t, const NfmKey& k, unsigned startFace, const NfmEpoch& epoch,
               long long nowTicks, long long serviceTicks, int status, int cause);
bool NfmErase(NfmTable* t, const NfmKey& k);
void NfmClear(NfmTable* t);

// Node-cap (3/3) or exhaustive unreachable (2) failures past the same iteration floor.
bool NfmShouldInsert(int status, int cause, int iterations);
// Bounded searches depend on positions that the key does not capture.
bool NfmUnreachableInputCovered(const void* input);
// Writes only the recorded status and cause into an initialized, empty output.
void NfmReplay(const NfmEntry& entry, void* output);
bool NfmWrongClass(int recordedStatus, int status);
// Observe's verdict on a would-hit: a real success means the memo would have been wrong.
bool NfmWrong(int status);
long long NfmTtlTicks(long long ticksPerSecond, bool doorSeen);

// Reads through the raw layouts, bounded where the engine is not. False when the section is past
// the collection or empty, or the face index is out of its array; *cluster and *data are then -1.
// *data is -1 too when the instance carries no face data.
bool NfmReadFace(const void* collection, unsigned faceKey, int* cluster, int* data);
// A hash of the slot count and, per slot, the instance, its original face array, its original face
// count and its runtime id. *ok is false when the count is out of range.
unsigned NfmSectionsFingerprint(const void* collection, bool* ok);

#endif // KEO_NPC_FAIL_MEMO_POLICY_H
