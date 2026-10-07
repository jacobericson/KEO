#include "fixes/physx/hull_same_skip_policy.h"

#include <string.h>

// The table's home slot for a hull address.
static unsigned SlotOf(uintptr_t key, unsigned mask)
{
	unsigned long long v = (unsigned long long)key;
	v ^= v >> 29;
	v *= 0x9E3779B97F4A7C15ULL;
	return (unsigned)(v >> 32) & mask;
}

// Linear probe from the home slot over at most min(HULL_SAME_PROBES, count)
// slots: *found is the key's entry, else *freeSlot the first free one in the
// window (either may stay NULL).
static void Probe(HullSameTable* t, uintptr_t hull, HullSameEntry** found, HullSameEntry** freeSlot)
{
	*found = NULL;
	*freeSlot = NULL;
	const unsigned mask = t->count - 1;
	const unsigned limit = t->count < HULL_SAME_PROBES ? t->count : HULL_SAME_PROBES;
	unsigned i = SlotOf(hull, mask);
	for (unsigned n = 0; n < limit; ++n, i = (i + 1) & mask)
	{
		HullSameEntry* e = &t->slots[i];
		if (e->hull == hull)
		{
			*found = e;
			return;
		}
		if (e->hull == 0)
		{
			*freeSlot = e;
			return;
		}
	}
}

void HullSameInit(HullSameTable* t, HullSameEntry* mem, unsigned n)
{
	t->slots = mem;
	t->count = n;
	t->used = 0;
	memset(mem, 0, (size_t)n * sizeof(HullSameEntry));
}

void HullSameClear(HullSameTable* t)
{
	memset(t->slots, 0, (size_t)t->count * sizeof(HullSameEntry));
	t->used = 0;
}

HullApplyAction HullSameStep(HullSameTable* t, uintptr_t hull, uintptr_t actor, bool teleport,
                             const unsigned pos[3], bool* emptied)
{
	*emptied = false;

	HullSameEntry* found;
	HullSameEntry* freeSlot;
	Probe(t, hull, &found, &freeSlot);

	// A create stores no target, and forgets the hull's old one: the new
	// actor may reuse the freed actor's address.
	if (actor == 0)
	{
		if (found)
			found->actor = 0;
		return HULL_FORWARD_CREATE;
	}

	if (!teleport && found && found->actor == actor
	    && found->pos[0] == pos[0] && found->pos[1] == pos[1] && found->pos[2] == pos[2])
		return HULL_SKIP;

	const HullApplyAction action = teleport ? HULL_FORWARD_TELEPORT : HULL_FORWARD_MOVE;

	if (!found && t->used >= t->count / 2)
	{
		HullSameClear(t);
		*emptied = true;
		Probe(t, hull, &found, &freeSlot);
	}

	HullSameEntry* e = found;
	if (!e && freeSlot)
	{
		e = freeSlot;
		e->hull = hull;
		++t->used;
	}
	if (e)
	{
		e->actor = actor;
		e->pos[0] = pos[0];
		e->pos[1] = pos[1];
		e->pos[2] = pos[2];
	}
	return action;
}

void HullSameRead(const void* hull, uintptr_t* actor, bool* teleport, unsigned pos[3])
{
	const unsigned char* h = (const unsigned char*)hull;
	*actor = *(const uintptr_t*)(h + HULL_OFF_ACTOR);
	*teleport = h[HULL_OFF_TELEPORT] != 0;
	memcpy(pos, h + HULL_OFF_POS, 12);
}

bool HullSameThunkOk(const unsigned char* thunk, uintptr_t thunkAddr, uintptr_t applyAddr)
{
	if (thunk[0] != 0xE9)
		return false;
	int rel;
	memcpy(&rel, thunk + 1, sizeof(rel));
	return thunkAddr + 5 + (intptr_t)rel == applyAddr;
}
