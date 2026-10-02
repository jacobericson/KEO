// char_movement_fields.h - Character order-cache and HavokCharacter field offsets.
// Included through game.h.

#ifndef KEO_CHAR_MOVEMENT_FIELDS_H
#define KEO_CHAR_MOVEMENT_FIELDS_H

#include "game/klib_members.h"
#include "base/core.h"

// ---- CharMovement order-cache and HavokCharacter fields ----
//
// HavokCharacter fields read from CharMovement's HavokCharacter pointer
// (OFF_CMOV_HAVOK_CHAR, +0x320): `+136` = arrival/idle code, `+144` = path
// state; OFF_HC_PATH_STATE is decimal 144 = 0x90.
// OFF_HC_PATH_STATE 3 is not "in progress": the engine's CharMovement::pathFailed
// (0x65DDA0) keys on it. Its raw test, on the CharMovement* cm:
//   hc = cm+0x320; return 0 if hc == NULL or the dword at cm+0x378 != 0;
//   return 0 if hc+0x90 != 3;
//   return 0 if the byte at cm+0x370 is set and the dword at cm+0x368 < 16;
//   else return 1.
// So 3 is the value pathFailed reports as failed; the +0x378 / +0x370 / +0x368
// conditions are stated as read, with no meaning assigned.
const size_t OFF_HC_PATH_STATE = 0x90;  // int: 3 = the value CharMovement::pathFailed reports as failed
KLIB_ASSERT_OFFSET(HavokCharacter_pathState, OFF_HC_PATH_STATE);
const size_t OFF_HC_ARRIVAL    = 136;   // int: arrival/idle code (<=1 idle)
KLIB_ASSERT_OFFSET(HavokCharacter_characterState, OFF_HC_ARRIVAL);

// Current-order-type chain from Character::playerMoveOrderDefault (0x5D1820),
// verified in IDA. Four dereferences, each must be null-checked:
//   pendingTaskListHead = *(uintptr_t*)(character + OFF_CHAR_PENDING_TASK_PTR)
//   pendingTask         = *(uintptr_t*)(pendingTaskListHead + OFF_PENDING_TASK_HEAD_OFF)
//   orderObj            = *(uintptr_t*)(pendingTask + OFF_PENDING_TASK_ORDER_OFF)
//   orderType           = *(int*)(orderObj + OFF_ORDER_TYPE)
// A null link at any step means "no cached order" -- playerMoveOrderDefault
// then always takes the fresh-AddOrder path, not the in-place rewrite.
const size_t OFF_CHAR_PENDING_TASK_PTR  = 1608;
KLIB_ASSERT_OFFSET(Character_body, OFF_CHAR_PENDING_TASK_PTR);
const size_t OFF_PENDING_TASK_HEAD_OFF  = 104;
KLIB_ASSERT_OFFSET(CharBody_currentAction, OFF_PENDING_TASK_HEAD_OFF);
const size_t OFF_PENDING_TASK_ORDER_OFF = 112;
KLIB_ASSERT_OFFSET(Tasker_taskData, OFF_PENDING_TASK_ORDER_OFF);
const size_t OFF_ORDER_TYPE             = 68;
KLIB_ASSERT_OFFSET(TaskData_key, OFF_ORDER_TYPE);
const int    ORDER_TYPE_MOVE            = 29;
// ---- end CharMovement order-cache and HavokCharacter fields ----

#endif // KEO_CHAR_MOVEMENT_FIELDS_H
