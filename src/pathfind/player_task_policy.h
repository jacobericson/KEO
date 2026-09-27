#include "game/klib_members.h"
#pragma once
#include <stdint.h>
#include <stddef.h>

static const size_t PT_OFF_TASKER_DATA    = 0x70;  // TaskData*: 0x50DB20 *(tasker+112)
KLIB_ASSERT_OFFSET(Tasker_taskData, PT_OFF_TASKER_DATA);
static const size_t PT_OFF_TASKDATA_TYPE  = 0x44;  // int TaskType: 0x50DB20 *(TaskData+68) == 187
KLIB_ASSERT_OFFSET(TaskData_key, PT_OFF_TASKDATA_TYPE);
static const int    PT_TYPE_LIMIT         = 300;   // task/goal types outside [0, 300) are rejected
static const int PT_NA = (-2147483647 - 1);        // field unreadable (null link / rejected): prints "-"

// Character / CharBody / AI / AITaskSystem / CharMovement / HavokCharacter /
// CombatClass / Blackboard offsets read by the PLAYER TASK diagnostic
// (pathfind_diag.cpp) and shared with islands.cpp. Every offset is a plain
// load, with the RVA that proves it; KLIB_ASSERT_OFFSET pins each to the
// KenshiLib member.

// Character
static const size_t PT_OFF_CHAR_HIT       = 0x2B0; // u8 under melee attack now: 0x435430 reads (AI+0x2F8 = Character)+688
KLIB_ASSERT_OFFSET(Character__isLiterallyUnderMeleeAttackRightNowForSure, PT_OFF_CHAR_HIT);
static const size_t PT_OFF_CHAR_MOVEMENT  = 0x640; // CharMovement*: 0x510460 l.51 calls *(Character+1600) vt+0x98 (stop)
KLIB_ASSERT_OFFSET(Character_movement, PT_OFF_CHAR_MOVEMENT);
static const size_t PT_OFF_CHAR_BODY      = 0x648; // CharBody*: 0x5C8820 reads *(Character+1608)
KLIB_ASSERT_OFFSET(Character_body, PT_OFF_CHAR_BODY);
static const size_t PT_OFF_CHAR_AI        = 0x650; // AI*: 0x5C7C70 getSensoryData = *(Character+1616)+0x28
KLIB_ASSERT_OFFSET(Character_ai, PT_OFF_CHAR_AI);
// CharBody
static const size_t PT_OFF_BODY_COMBAT    = 0x08;  // CombatClass*: 0x5C8820 returns *(CharBody+8)
KLIB_ASSERT_OFFSET(CharBody_combatClass, PT_OFF_BODY_COMBAT);
static const size_t PT_OFF_BODY_ACTION    = 0x68;  // current action Tasker*: 0x5C6430 setCurrentTask, 0x5D1820
KLIB_ASSERT_OFFSET(CharBody_currentAction, PT_OFF_BODY_ACTION);
// AI (SensoryData embedded at AI+0x28, 0x5C7C70)
static const size_t PT_OFF_AI_PLATOON     = 0x10;  // 0x5065E0: *(AI+16), its +0xF8 is the Blackboard
KLIB_ASSERT_OFFSET(AI_platoon, PT_OFF_AI_PLATOON);
static const size_t PT_OFF_AI_TASKSYS     = 0x20;  // AITaskSystem*: 0x510820 passes AI[4] down the think chain
KLIB_ASSERT_OFFSET(AI_taskSystemAI, PT_OFF_AI_TASKSYS);
static const size_t PT_OFF_AI_NEAREST_SQ  = 0x28;  // float SensoryData.nearestEnemy (squared): 0x858500 keeps the minimum
KLIB_ASSERT_OFFSET(AI_sensoryData_nearestEnemy, PT_OFF_AI_NEAREST_SQ);
static const size_t PT_OFF_AI_THREATS     = 0x80;  // int threats count: 0x599290 *(AI+128); 0x8534A0 pushes at Sensory+0x50
KLIB_ASSERT_OFFSET(AI_sensoryData_threats_count, PT_OFF_AI_THREATS);
static const size_t PT_OFF_AI_THREAT_PERS = 0xB0;  // float totalThreatLevelPersonal: 0x8534A0 Sensory+136
KLIB_ASSERT_OFFSET(AI_sensoryData_totalThreatLevelPersonal, PT_OFF_AI_THREAT_PERS);
static const size_t PT_OFF_AI_NUM_ENEMIES = 0xBC;  // int numEnemies: 0x596BA0 NO_ENEMIES_IN_VICINITY reads AI+188
KLIB_ASSERT_OFFSET(AI_sensoryData_numEnemies, PT_OFF_AI_NUM_ENEMIES);
// AITaskSystem
static const size_t PT_OFF_TS_PERMAJOB_COUNT = 0x90;  // int permanent job count: 0x50DB20 *(ts+144)
KLIB_ASSERT_OFFSET(OrdersReceiver_permajobs_count, PT_OFF_TS_PERMAJOB_COUNT);
static const size_t PT_OFF_TS_PERMAJOB_ARRAY = 0x98;  // Tasker** permanent jobs: 0x50DB20 *(ts+152)
KLIB_ASSERT_OFFSET(OrdersReceiver_permajobs_stuff, PT_OFF_TS_PERMAJOB_ARRAY);
// u64 size of the player-order std::deque that the order-deletion function
// pops: 0x50CD40 calls 0x518210 (thunk 0x1A181) on ts+0x38; 0x518210 returns
// 0 when *(obj+40) == 0, else pops the deque at obj+8 and decrements its size
// v1[4] = obj+8+32 = obj+40 -> ts+0x60.
static const size_t PT_OFF_TS_DEQUE_SIZE  = 0x60;
KLIB_ASSERT_OFFSET(OrdersReceiver_orders_list__Mysize, PT_OFF_TS_DEQUE_SIZE);
static const size_t PT_OFF_TS_GOAL        = 0x1C0; // inline TaskMatch; first member const TaskData* (legacy stages read Tasker*)
KLIB_ASSERT_OFFSET(OrdersReceiver_currentGoal_taskData, PT_OFF_TS_GOAL);
static const size_t PT_OFF_TS_GOAL_LEVEL  = 0x20C; // int level of the current goal: 0x510460 ts+524 gates the cascade
KLIB_ASSERT_OFFSET(OrdersReceiver_currentGoalPriority, PT_OFF_TS_GOAL_LEVEL);
static const size_t PT_OFF_TS_FINISHED    = 0x26C; // u8 current action finished: 0x50BCA0 writes ts+620 = 1
KLIB_ASSERT_OFFSET(AITaskSytem__taskCompletedFlag, PT_OFF_TS_FINISHED);
static const size_t PT_OFF_TS_IMPOSSIBLE  = 0x26D; // u8 currentTaskImpossible: 0x50BE00 writes ts+621 = 1
KLIB_ASSERT_OFFSET(AITaskSytem__taskImpossibleFlag, PT_OFF_TS_IMPOSSIBLE);
static const int    PT_PERMAJOB_COUNT_LIMIT  = 1000;  // permanent job counts outside [0, 1000) are rejected
// CharMovement
static const size_t PT_OFF_CMOV_STOPPED   = 0x08;  // u8 officiallyStopped: stop() 0x65F1E0 writes 1
KLIB_ASSERT_OFFSET(AbstractMovementBase_officiallyStopped, PT_OFF_CMOV_STOPPED);
static const size_t PT_OFF_CMOV_MOVING    = 0x24;  // u8 currentlyMoving: 0x65E320 this+36
KLIB_ASSERT_OFFSET(AbstractMovementBase_currentlyMoving, PT_OFF_CMOV_MOVING);
static const size_t PT_OFF_CMOV_HC        = 0x320; // HavokCharacter*: 0x65E320 this+800
KLIB_ASSERT_OFFSET(CharMovement_havokCharacter, PT_OFF_CMOV_HC);
static const size_t PT_OFF_CMOV_EDGE_CTR  = 0x368; // int edge retry counter: 0x65DDA0 pathFailed (< 16)
KLIB_ASSERT_OFFSET(CharMovement_edgeTarget, PT_OFF_CMOV_EDGE_CTR);
static const size_t PT_OFF_CMOV_EDGE      = 0x370; // u8 movingToEdge: 0x65E320 this+880
KLIB_ASSERT_OFFSET(CharMovement_movingToEdge, PT_OFF_CMOV_EDGE);
// HavokCharacter
static const size_t PT_OFF_HC_ARRIVAL     = 0x88;  // int arrival code (1 = arrived): 0x65E320 hc+136 == 1
KLIB_ASSERT_OFFSET(HavokCharacter_characterState, PT_OFF_HC_ARRIVAL);
static const size_t PT_OFF_HC_PATH_STATE  = 0x90;  // int path state (3 = failed): 0x65DDA0 hc+144 == 3
KLIB_ASSERT_OFFSET(HavokCharacter_pathState, PT_OFF_HC_PATH_STATE);
// CombatClass
static const size_t PT_OFF_CC_STATE       = 0x1F0; // int combat state: 0x60C3A0 CombatClass::update this+496
static_assert(PT_OFF_CC_STATE == KLIB_OFF_CombatClass_combatState, "PT_OFF_CC_STATE composed parity");
static const size_t PT_OFF_CC_ATTACKERS   = 0x200; // int attackersH count (KenshiLib layout, header-verified)
static_assert(PT_OFF_CC_ATTACKERS == KLIB_OFF_CombatClass_attackersH_count, "PT_OFF_CC_ATTACKERS composed parity");
// Blackboard
static const size_t PT_OFF_PLATOON_BB     = 0xF8;  // Blackboard*: 0x5065E0 *(*(AI+16)+248)
KLIB_ASSERT_OFFSET(Platoon_blackboard, PT_OFF_PLATOON_BB);
static const size_t PT_OFF_BB_REQ_SIZE    = 0x178; // u64 TaskRequest map size: 0x269200, ctor 0x26BCB0
static_assert(PT_OFF_BB_REQ_SIZE == KLIB_OFF_Blackboard_requests + KLIB_OFF_RequestMapTable_size_, "PT_OFF_BB_REQ_SIZE composed parity");

// Macros keep all pointer reads lexically inside the caller's SEH guard.
#define PT_TASKDATA_TYPE(dataExpr, outField) \
	do { \
		uintptr_t ptData_ = (dataExpr); \
		(outField) = -1; \
		if (ptData_) { \
			int ptType_ = *(int*)(KLIB_MEMBER(3, ptData_, TaskData_key, PT_OFF_TASKDATA_TYPE)); \
			(outField) = (ptType_ < 0 || ptType_ >= PT_TYPE_LIMIT) ? PT_NA : ptType_; \
		} \
	} while (0)

#define PT_TASKER_TYPE(taskerExpr, outField)                                   \
	do {                                                                       \
		uintptr_t ptTk_ = (taskerExpr);                                        \
		(outField) = -1;                                                       \
		if (ptTk_) {                                                           \
			uintptr_t ptTd_ = *(uintptr_t*)(KLIB_MEMBER(3, ptTk_, Tasker_taskData, PT_OFF_TASKER_DATA));       \
			if (ptTd_) {                                                       \
				int ptTy_ = *(int*)(KLIB_MEMBER(3, ptTd_, TaskData_key, PT_OFF_TASKDATA_TYPE));             \
				(outField) = (ptTy_ < 0 || ptTy_ >= PT_TYPE_LIMIT) ? PT_NA : ptTy_; \
			}                                                                  \
		}                                                                      \
	} while (0)

#define PT_GOAL_TYPE(dataExpr, outField) PT_TASKDATA_TYPE(dataExpr, outField)
