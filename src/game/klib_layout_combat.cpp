#include "base/klib_include.h"
#include <Windows.h>
#include <stddef.h>
#include <kenshi/combat/CombatClass.h>
static_assert(offsetof(CombatClass,combatState)==0x1F0,"combatState");
static_assert(offsetof(CombatClass,attackersH)==0x1F8,"attackersH");

#define KLIB_FIELD(N,T,M,O,W) static_assert(offsetof(T,M)==O, #N " offset"); static_assert(sizeof(((T*)0)->M)==W, #N " width");
#include "game/klib_member_combat.inc"
#undef KLIB_FIELD
#include "game/klib_members.h"
#define KLIB_FIELD(N,T,M,O,W) uintptr_t KlibField_##N(uintptr_t base) { return (uintptr_t)&((T*)base)->M; }
#include "game/klib_member_combat.inc"
#undef KLIB_FIELD
#include "base/klib_include_end.h"
