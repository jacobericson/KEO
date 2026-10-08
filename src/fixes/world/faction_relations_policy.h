#ifndef KEO_FIXES_FACTION_RELATIONS_POLICY_H
#define KEO_FIXES_FACTION_RELATIONS_POLICY_H

// The FactionRelations map lookup the update detour uses in place of the
// engine's walk, the walk it is checked against, and the switch rules. Pure:
// no game or Windows header; the lookup reads only the memory it is handed.

#include <stddef.h>
#include <stdint.h>

// A FactionRelations object's fields: the faction itself, and its boost map's
// table (bucket count, size, bucket array).
const size_t REL_OBJ_ME           = 0x08;
const size_t REL_OBJ_BUCKET_COUNT = 0x38;
const size_t REL_OBJ_SIZE         = 0x40;
const size_t REL_OBJ_BUCKETS      = 0x58;

// A map node from its own address: the link to the next node, the hash it
// was stored under, the key (a Faction*), and the relation float of its
// RelationData. A bucket holds the link of the node before its first node.
const size_t REL_NODE_NEXT     = 0x00;
const size_t REL_NODE_HASH     = 0x08;
const size_t REL_NODE_KEY      = 0x10;
const size_t REL_NODE_RELATION = 0x1C;

// The bits of 100.0f, the value the engine writes into a faction's own entry.
const unsigned REL_SELF_VALUE_BITS = 0x42C80000u;

enum RelMode { REL_MODE_OFF = 0, REL_MODE_ON = 1, REL_MODE_VERIFY = 2 };

// The engine's hash of a Faction* key: boost's pointer hash, p + (p >> 3),
// then boost's 64-bit mix.
unsigned long long RelKeyHash(unsigned long long key);

struct RelTable
{
	uintptr_t buckets;
	size_t    bucketCount;
	size_t    size;
};

// REL_LIMIT: the table is not one this lookup understands (no buckets, a
// bucket count that is not a power of two, or a chain longer than the table).
enum RelFindResult { REL_FOUND, REL_ABSENT, REL_LIMIT };

// The engine's probe: the key's bucket, then its chain until a node of
// another bucket. *node is the node found, else 0.
RelFindResult RelLookup(const RelTable& t, uintptr_t key, uintptr_t* node);

// The engine's walk, read-only: every node from the sentinel bucket, the
// first whose key matches. *node is the node found, else 0.
RelFindResult RelWalkFind(const RelTable& t, uintptr_t key, uintptr_t* node);

// The lookup checked against the walk. REL_VERIFY_HASH: they differ and the
// walk's node was stored under another hash than RelKeyHash gives its key.
enum RelVerify { REL_VERIFY_OK, REL_VERIFY_NODE, REL_VERIFY_HASH };
RelVerify RelVerifyLookup(uintptr_t lookupNode, uintptr_t walkNode, unsigned long long walkStoredHash,
                          unsigned long long keyHash);

// The engine's write: 100.0f into the node's relation, four bytes.
void RelWriteSelf(uintptr_t node);

// What the update detour does with one call. inWindow: the call falls inside
// the verified stretch after a switch to on.
enum RelPath { REL_PATH_OFF, REL_PATH_FORWARD, REL_PATH_LOOKUP, REL_PATH_VERIFY };
RelPath RelChoosePath(int mode, bool fallback, uintptr_t me, bool inWindow);

// A switch to on (from off or verify) opens a verified stretch.
bool RelArmVerifyWindow(int oldMode, int newMode);

#endif // KEO_FIXES_FACTION_RELATIONS_POLICY_H
