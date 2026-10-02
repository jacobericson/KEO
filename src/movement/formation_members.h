// formation_members.h — Pure formation-member bookkeeping (Layer 0, host-testable)
// No game or KenshiLib headers: matching and completion logic only.

#ifndef KEO_FORMATION_MEMBERS_H
#define KEO_FORMATION_MEMBERS_H

#include <stdint.h>

// Zero characters[i] and movements[i] for every i in [0, count) whose
// characters[i] equals one of chars[0, n). A slot already 0 never matches
// (0 is never a live character), so re-detaching an already-zeroed slot is
// a no-op. Returns the number of slots zeroed.
int FormationZeroMatchingMembers(uintptr_t* characters, uintptr_t* movements,
                                  int count, const uintptr_t* chars, int n);

// True when every slot in characters[0, count) is 0 (including count == 0):
// the group's membership is exhausted and should be retired.
bool FormationGroupExhausted(const uintptr_t* characters, int count);

// True if the live (non-zero) entries of characters[0, count) are exactly
// chars[0, n) as sets, order-independent: same size and every live entry
// found in chars[]. A subset or superset is never a match.
bool FormationSameMemberSet(const uintptr_t* characters, int count,
                             const uintptr_t* chars, int n);

// True only when a member's Hold or inSomething state has gone from off (at
// group creation) to on now. A member that was already holding or in
// something (a bed, a chair, a cage) when the group formed keeps traveling
// with it; only a state picked up after the order was given detaches it.
bool FormationMemberNewlyHeld(bool holdAtCreation, bool holdNow,
                               bool inSomethingAtCreation, bool inSomethingNow);

// Gather-completion radius (world units) for a group of `memberCount`
// characters: grows with squad size (sqrt(memberCount * 60) + 10) so a large
// squad's stragglers aren't held to the same radius as a handful of
// characters, floored at 40 units so a small squad is unchanged.
float FormationGatherRadius(int memberCount);

// Largest XZ distance between any two live members of xs/zs[0, count), where
// a live member is one whose characters[i] is non-zero. Fewer than two live
// members is 0.0f: one member cannot be spread out, and neither can none.
// That makes 0.0f mean "nothing to measure or every member on one spot" --
// both indistinguishable from the caller's point of view and both equally
// "together" -- while any positive value is a real separation in world units.
float FormationMaxPairwiseSpread(const float* xs, const float* zs,
                                  const uintptr_t* characters, int count);

#endif // KEO_FORMATION_MEMBERS_H
