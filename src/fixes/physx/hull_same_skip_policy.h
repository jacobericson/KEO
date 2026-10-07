#ifndef KEO_FIXES_HULL_SAME_SKIP_POLICY_H
#define KEO_FIXES_HULL_SAME_SKIP_POLICY_H

// Which click-hull applies the physics thread may skip: the target last
// forwarded for the same hull and actor, bit for bit, with no teleport
// latched and an actor already made. One writer (the physics thread). Pure:
// no game or Windows header.

#include <stddef.h>
#include <stdint.h>

// A PhysicsHullT's fields the apply reads: the latched teleport byte, the
// position it forwards (three floats) and its PhysX actor.
const size_t HULL_OFF_TELEPORT = 0x34;
const size_t HULL_OFF_POS      = 0x38;
const size_t HULL_OFF_ACTOR    = 0x50;

const unsigned HULL_SAME_SLOTS  = 16384;  // a power of two
const unsigned HULL_SAME_PROBES = 32;     // slots one lookup reads at most

struct HullSameEntry
{
	uintptr_t hull;      // 0 = free
	uintptr_t actor;
	unsigned  pos[3];    // the forwarded target's bits
	unsigned  pad;
};

struct HullSameTable
{
	HullSameEntry* slots;
	unsigned       count;   // a power of two
	unsigned       used;
};

enum HullApplyAction { HULL_FORWARD_CREATE, HULL_FORWARD_TELEPORT, HULL_FORWARD_MOVE, HULL_SKIP };

void HullSameInit(HullSameTable* t, HullSameEntry* mem, unsigned n);
void HullSameClear(HullSameTable* t);

// One apply's decision, and what the table keeps of it: a create stores
// no target and clears the hull's stored actor, so no later move skips
// against a target from before it; a teleport or a move stores the target
// for (hull, actor), first emptying the table when half full (*emptied
// set); a skip changes nothing.
HullApplyAction HullSameStep(HullSameTable* t, uintptr_t hull, uintptr_t actor, bool teleport,
                             const unsigned pos[3], bool* emptied);

// The apply's three inputs, read from a hull.
void HullSameRead(const void* hull, uintptr_t* actor, bool* teleport, unsigned pos[3]);

// The thunk in the slot is a five-byte E9 whose target is the apply.
bool HullSameThunkOk(const unsigned char* thunk, uintptr_t thunkAddr, uintptr_t applyAddr);

#endif // KEO_FIXES_HULL_SAME_SKIP_POLICY_H
