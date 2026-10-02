// formation_internal.h - Shared formation lifecycle predicates.
// Main thread; implementations stay in the core and take no formation lock.

#ifndef KEO_FORMATION_INTERNAL_H
#define KEO_FORMATION_INTERNAL_H

#include "movement/formation.h"

namespace formation_detail {

bool CharacterNewlyHeld(uintptr_t character, bool holdAtCreation, bool inSomethingAtCreation);
void DeactivateFormationGroup(int g);

} // namespace formation_detail
using namespace formation_detail;

#endif // KEO_FORMATION_INTERNAL_H
