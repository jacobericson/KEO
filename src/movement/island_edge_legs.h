#ifndef KEO_ISLAND_EDGE_LEGS_H
#define KEO_ISLAND_EDGE_LEGS_H

// What a tracked player order does after the far-span rule sends it down the
// edge-route branch: whether it enters edge mode, how far each leg gets it,
// whether it parks, how far the engine's retry ladder climbs, and whether it
// hands over to a direct path near the destination or arrives far short.
//
// The order set is the orders whose own cell and destination cell satisfy the
// rule's predicate at some frame. With the rule off the same predicate at span
// 2 selects the orders it would have moved, so a control run reports the same
// fields for the same class of order.
//
// Main thread only (SamplePlayerArrivals, PLAYER STUCK, IslandTick).

#include <stddef.h>
#include <sstream>
#include "base/config.h"


void EdgeLegsForget(int slot);   // a new or re-clicked order in that tracked slot

// Per frame for one live tracked player. hc136/moving/edge are the values the
// arrival sampler already read.
void EdgeLegsSample(int slot, uintptr_t cm, int hc136, bool moving, bool edge,
                    float posX, float posZ, float destX, float destZ, double now);

void EdgeLegsAppendStuck(int slot, std::ostringstream& ss);   // PLAYER STUCK suffix
void EdgeLegsAppendSummary(std::ostringstream& ss);           // IslandSpan: suffix


#endif // KEO_ISLAND_EDGE_LEGS_H
