// audit_pose_table.h - The pose each PhysX hull was last given, and the class of the next one: a
// fixed open-addressed table keyed by hull address, with one writer. Pure: no Windows or game
// header, so the host tests build it alone.

#ifndef KENSHI_FRAME_AUDIT_POSE_TABLE_H
#define KENSHI_FRAME_AUDIT_POSE_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace hullpose
{

// A submission's class, in precedence order: no actor yet, a teleport, no stored pose, the stored
// pose exactly, a move shorter than TINY_MOVE, anything else.
enum PoseClass { PC_CREATE, PC_TELEPORT, PC_FIRST, PC_SAME, PC_TINY, PC_MOVED, PC_COUNT };

const unsigned TABLE_SLOTS = 16384;   // a power of two
const unsigned PROBE_LIMIT = 32;      // slots one lookup reads at most
const float    TINY_MOVE   = 0.01f;   // world units

struct Entry
{
	uintptr_t key;   // hull address; 0 = free
	float     x, y, z;
};

inline unsigned SlotOf(uintptr_t key, unsigned mask)
{
	unsigned long long v = (unsigned long long)key;
	v ^= v >> 29;
	v *= 0x9E3779B97F4A7C15ULL;
	return (unsigned)(v >> 32) & mask;
}

struct Table
{
	Entry*   slots;
	unsigned count;    // a power of two
	unsigned used;     // keys stored
	unsigned clears;

	void Init(Entry* mem, unsigned n)
	{
		slots  = mem;
		count  = n;
		used   = 0;
		clears = 0;
		memset(mem, 0, sizeof(Entry) * n);
	}

	// Empties the table once half its slots hold a key: hulls are destroyed and their addresses
	// reused, and a table that only grew would fill with them. True when it emptied it.
	bool ClearIfHalfFull()
	{
		if (used < count / 2)
			return false;
		memset(slots, 0, sizeof(Entry) * count);
		used = 0;
		++clears;
		return true;
	}

	// The class of one submission; the submitted pose is then stored for the hull. A lookup that
	// meets neither its key nor a free slot within PROBE_LIMIT stores nothing.
	int Classify(uintptr_t hull, bool hasActor, bool teleport, float x, float y, float z)
	{
		if (!hull)
			return !hasActor ? PC_CREATE : (teleport ? PC_TELEPORT : PC_FIRST);
		Entry* slot = NULL;
		bool known = false;
		unsigned mask = count - 1;
		unsigned i = SlotOf(hull, mask);
		for (unsigned n = 0; n < PROBE_LIMIT && n < count; ++n, i = (i + 1) & mask)
		{
			if (slots[i].key == hull)
			{
				slot = &slots[i];
				known = true;
				break;
			}
			if (slots[i].key == 0)
			{
				slot = &slots[i];
				break;
			}
		}
		int cls;
		if (!hasActor)
			cls = PC_CREATE;
		else if (teleport)
			cls = PC_TELEPORT;
		else if (!known)
			cls = PC_FIRST;
		else
		{
			float dx = x - slot->x, dy = y - slot->y, dz = z - slot->z;
			if (dx == 0.0f && dy == 0.0f && dz == 0.0f)
				cls = PC_SAME;
			else if (dx * dx + dy * dy + dz * dz < TINY_MOVE * TINY_MOVE)
				cls = PC_TINY;
			else
				cls = PC_MOVED;
		}
		if (slot)
		{
			if (!known)
			{
				slot->key = hull;
				++used;
			}
			slot->x = x;
			slot->y = y;
			slot->z = z;
		}
		return cls;
	}
};

} // hullpose

#endif
