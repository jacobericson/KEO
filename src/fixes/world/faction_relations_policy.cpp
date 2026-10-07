// faction_relations_policy.cpp - The FactionRelations map lookup, the walk it is checked against, and the switch rules.
#include "fixes/world/faction_relations_policy.h"

// p + (p >> 3), then ~k + (k << 21), ^ >> 24, * 265, ^ >> 14, * 21, ^ >> 28, * 0x80000001.
unsigned long long RelKeyHash(unsigned long long key)
{
	unsigned long long k = key + (key >> 3);
	k = ~k + (k << 21);
	k ^= k >> 24;
	k = k * 265;
	k ^= k >> 14;
	k = k * 21;
	k ^= k >> 28;
	k = k + (k << 31);
	return k;
}

// The key's bucket, then its chain: a node with the key's hash and the key is
// the entry; a node stored under another bucket ends the chain. More nodes
// than the table holds is a cycle or a table this lookup does not understand.
RelFindResult RelLookup(const RelTable& t, uintptr_t key, uintptr_t* node)
{
	*node = 0;
	if (t.size == 0)
		return REL_ABSENT;
	if (!t.buckets || t.bucketCount == 0 || (t.bucketCount & (t.bucketCount - 1)))
		return REL_LIMIT;

	const unsigned long long h = RelKeyHash((unsigned long long)key);
	const unsigned long long mask = (unsigned long long)(t.bucketCount - 1);
	const unsigned long long b = h & mask;
	const uintptr_t prev = *(const uintptr_t*)(t.buckets + (size_t)b * 8);
	if (!prev)
		return REL_ABSENT;

	size_t steps = 0;
	uintptr_t n = *(const uintptr_t*)(prev + REL_NODE_NEXT);
	while (n)
	{
		if (++steps > t.size)
			return REL_LIMIT;
		const unsigned long long nh = *(const unsigned long long*)(n + REL_NODE_HASH);
		if (nh == h && *(const uintptr_t*)(n + REL_NODE_KEY) == key)
		{
			*node = n;
			return REL_FOUND;
		}
		if (nh != h && (nh & mask) != b)
			return REL_ABSENT;
		n = *(const uintptr_t*)(n + REL_NODE_NEXT);
	}
	return REL_ABSENT;
}

// Every node from the sentinel bucket's link, the first with the key; more
// nodes than the table holds is refused.
RelFindResult RelWalkFind(const RelTable& t, uintptr_t key, uintptr_t* node)
{
	*node = 0;
	if (t.size == 0)
		return REL_ABSENT;
	if (!t.buckets)
		return REL_LIMIT;

	size_t steps = 0;
	uintptr_t n = *(const uintptr_t*)(t.buckets + t.bucketCount * 8);
	while (n)
	{
		if (++steps > t.size)
			return REL_LIMIT;
		if (*(const uintptr_t*)(n + REL_NODE_KEY) == key)
		{
			*node = n;
			return REL_FOUND;
		}
		n = *(const uintptr_t*)(n + REL_NODE_NEXT);
	}
	return REL_ABSENT;
}

// Equal nodes match; otherwise a walk node stored under another hash than the
// key's is a hash mismatch, and anything else a node mismatch.
RelVerify RelVerifyLookup(uintptr_t lookupNode, uintptr_t walkNode, unsigned long long walkStoredHash,
                          unsigned long long keyHash)
{
	if (lookupNode == walkNode)
		return REL_VERIFY_OK;
	if (walkNode && walkStoredHash != keyHash)
		return REL_VERIFY_HASH;
	return REL_VERIFY_NODE;
}

// One four-byte store of 100.0f's bits at the node's relation.
void RelWriteSelf(uintptr_t node)
{
	*(volatile unsigned*)(node + REL_NODE_RELATION) = REL_SELF_VALUE_BITS;
}

// Off forwards untouched; a fallback or a NULL faction forwards; verify, or on
// inside the window, verifies; on looks up. A mode other than 0-2 reads as on.
RelPath RelChoosePath(int mode, bool fallback, uintptr_t me, bool inWindow)
{
	if (mode == REL_MODE_OFF)
		return REL_PATH_OFF;
	if (fallback || !me)
		return REL_PATH_FORWARD;
	if (mode == REL_MODE_VERIFY || inWindow)
		return REL_PATH_VERIFY;
	return REL_PATH_LOOKUP;
}

// Only a change into on, from any other mode, arms the window.
bool RelArmVerifyWindow(int oldMode, int newMode)
{
	return newMode == REL_MODE_ON && oldMode != REL_MODE_ON;
}

// With the guard on, a non-NULL source equal to the faction itself is dropped.
bool RelGuardDrops(int guard, uintptr_t me, uintptr_t from)
{
	return guard != 0 && from != 0 && from == me;
}
