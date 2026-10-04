// order_hook.cpp - Player move-order capture and reprioritization.
// Main thread; queue reprioritization takes the navmesh queue lock alone.

#include "pathfind/order_hook.h"
#include "zone/transition_hook.h"
#include "zone/preload/preload.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "pathfind/pathfinding.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "movement/islands.h"
#include "movement/order_outcome.h"
#include "bench/bench_runner.h"
#include "planner/planner_tick.h"

// =========================================================================
// Hook 4: addOrderSelectedCharacters -- player move order capture
// =========================================================================
//
// x64 MSVC thiscall: RCX=this, RDX=destIndoors, R8=task, R9=subject,
// stack: shift, addDontClear, location (const Vector3& = float*)
//
// For task==29 (MOVE_TO), we iterate the selected characters linked list
// (same structure the original function uses), register cross-zone movers
// into the watched array with the destination zone, then call the original.
// Edge detection (~1s poll) handles actual zone preloading as characters
// approach boundaries.
//
// Selected characters linked list layout (from decompiled addOrderSelectedCharacters):
//   count:    *(uint64_t*)(thisPI + 552)
//   arrayPtr: *(uint64_t*)(thisPI + 576)
//   index:    *(uint64_t*)(thisPI + 544)
//   head:     *(node**)(arrayPtr + 8 * index)
//   iterate:  node = *(node*)*node  (next pointer at +0)
//   type:     *(int*)(node + 24)    (1 = character handle)
//   handle:   (void*)(node + 16)    (hand::getCharacter)

#ifdef KEO_DEBUG
static void NoteMissingSelectedCharacter(void* character)
{
	static bool logged = false;
	if (!character && !logged)
	{
		logged = true;
		LogMsg("Selected hand has no Character; skipped (no platoon fallback)");
	}
}
#endif

// Main thread, from the non-move branch before the original: drops every selected character's
// island order, formation membership and route plan, and closes its order outcome as a cancel.
static void DropSelectedCharacterOrders(void* thisPI)
{
	uintptr_t piAddr = (uintptr_t)thisPI;
	uintptr_t count = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_count, OFF_PI_SEL_COUNT));
	if (count > 0)
	{
		uintptr_t arrayPtr = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_buckets, OFF_PI_SEL_ARRAY));
		uintptr_t index    = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_bucketCount, OFF_PI_SEL_INDEX));
		if (arrayPtr && index < 1024)
		{
			uintptr_t* node = *(uintptr_t**)(arrayPtr + 8 * index);
			void* sentinel = *(void**)(gameBase + RVA_HANDLE_SENTINEL);
			int maxIter = (int)count + 16;
			int iter = 0;
			while (node)
			{
				if (++iter > maxIter) break;
				int nodeType = *(int*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_handle_type, OFF_SEL_NODE_TYPE));
				if (nodeType == 1)
				{
					void* resolved = KlibSelectedCharacter((const void*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_value_base_, OFF_SEL_NODE_HANDLE)));
					uintptr_t character = (uintptr_t)resolved;
#ifdef KEO_DEBUG
					NoteMissingSelectedCharacter(resolved);
#endif
					if (character && (void*)character != sentinel)
					{
						IslandDropOrder(character);
						planner::PlannerDrop(character);
						// Any non-move order supersedes squad membership too
						// (attack, job, pick-up, talk): drop it from its
						// formation group the same way.
						FormationDetachCharacters(&character, 1);
						// A player-issued cancel, not a routing failure --
						// an open stall resolves as a user re-order.
						OrderOutcomeCancel(character, ElapsedSec());
					}
				}
				node = *(uintptr_t**)KLIB_MEMBER(3, node, HandSetNode_next_, 0);
			}
		}
	}
}

void hook_addOrderSelected(void* thisPI, void* destIndoors, int task,
                            void* subject, bool shift, bool addDontClear,
                            const float* location)
{
	BenchNotifyPlayerOrder();

	// Capture group BEFORE calling original
	int charsAdded = 0;

	// Collect all resolved characters for formation group + pending-order buffer
	uintptr_t collectedChars[MAX_FORMATION_MEMBERS];
	int collectedCount = 0;

	if (task == 29 && location
	)
	{
		float destX = (*(const float*)KLIB_MEMBER(5, location, Ogre__Vector3_x, 0));
		float destZ = (*(const float*)KLIB_MEMBER(5, location, Ogre__Vector3_z, 8));  // Ogre: Y is up, Z is horizontal

		int destGX = -1, destGY = -1;
		bool haveDest = gridCalibrated && WorldToZoneGrid(destX, destZ, &destGX, &destGY);

		// Iterate the selected characters linked list
		uintptr_t piAddr = (uintptr_t)thisPI;
		uintptr_t count = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_count, OFF_PI_SEL_COUNT));

		if (count > 0)
		{
			uintptr_t arrayPtr = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_buckets, OFF_PI_SEL_ARRAY));
			uintptr_t index    = *(uintptr_t*)(KLIB_MEMBER(3, piAddr, PlayerInterface_selected_bucketCount, OFF_PI_SEL_INDEX));

			if (arrayPtr && index < 1024)
			{
			uintptr_t* node    = *(uintptr_t**)(arrayPtr + 8 * index);
			void* sentinel = *(void**)(gameBase + RVA_HANDLE_SENTINEL);

			int maxIter = (int)count + 16;
			int iter = 0;
			while (node)
			{
				if (++iter > maxIter) break;
				int nodeType = *(int*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_handle_type, OFF_SEL_NODE_TYPE));
				if (nodeType == 1)
				{
					// Resolve handle at node+16 to get Character*
					void* resolved = KlibSelectedCharacter((const void*)(KLIB_MEMBER(3, (uintptr_t)node, HandSetNode_value_base_, OFF_SEL_NODE_HANDLE)));
					uintptr_t character = (uintptr_t)resolved;
#ifdef KEO_DEBUG
					NoteMissingSelectedCharacter(resolved);
#endif

					if (character && (void*)character != sentinel)
					{
						// Collect for formation group + pending-order buffer
						if (collectedCount < MAX_FORMATION_MEMBERS)
							collectedChars[collectedCount++] = character;
						// Island re-issue tracker
						IslandNoteOrder(character, location);

						// Movement-aware preload tracking: cross-zone movers
						if (zone::g_zoneCfg.preloadEnabled && zone::g_zoneCfg.movementAwareEnabled && haveDest)
						{
							float charX = GetCharPosX(character);
							float charZ = GetCharPosZ(character);

							int curGX, curGY;
							if (WorldToZoneGrid(charX, charZ, &curGX, &curGY))
							{
								if (curGX != destGX || curGY != destGY)
								{
									uintptr_t charMov = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
									if (charMov)
									{
										if (AddWatchedCharacter(character, charMov,
										                        destGX, destGY, curGX, curGY,
										                        /*hasMoveOrder=*/true))
										{
											charsAdded++;
										}
									}
								}
							}
						}
					}
				}

				// Follow next pointer at node+0
				node = *(uintptr_t**)KLIB_MEMBER(3, node, HandSetNode_next_, 0);
			}
			}
		}

		int lead = planner::PlannerNoteOrder(collectedChars, collectedCount, location, destIndoors, shift, addDontClear);

		// One order-outcome record per player move order, for every
		// selected character (not just the ones movement-aware preload
		// tracks). Main thread; the detour already logs and allocates above.
		if (collectedCount > 0)
			OrderOutcomeBegin(collectedChars, collectedCount, destX, destZ, ElapsedSec());

		hookOrderCount++;

		// One-time confirmation that scatter patch is exercised on multi-char orders
		if (scatterPatchApplied && count > 1)
		{
			static bool scatterLogOnce = false;
			if (!scatterLogOnce)
			{
				scatterLogOnce = true;
				std::ostringstream ss;
				ss << "Scatter patch active: " << count
				   << " chars ordered to (" << std::fixed << std::setprecision(0)
				   << destX << "," << destZ << ") — all receive exact dest";
				LogMsg(ss.str());
			}
		}

		if (charsAdded > 0)
		{
			std::ostringstream ss;
			ss << "Order captured: dest zone (" << destGX << "," << destGY
			   << ") " << charsAdded << " chars tracked";
			LogMsg(ss.str());
		}

		// Stuck recovery: store click destination per character
		if (location && collectedCount > 0)
		{
			double clickNow = ElapsedSec();
			for (int c = 0; c < collectedCount; ++c)
				StorePlayerClickDest(collectedChars[c], location, clickNow);
		}

		// Group cohesion: create formation group for arrival scatter.
		bool formedGroup = false;
		if (movement::g_movementCfg.groupCohesionEnabled && scatterPatchApplied && collectedCount > 1)
		{
			bool allGrouped = true;
			for (int c = 0; c < collectedCount; ++c)
			{
				uintptr_t cm = *(uintptr_t*)(KLIB_MEMBER(3, collectedChars[c], Character_movement, OFF_CHAR_MOVEMENT));
				if (!cm || *(int*)(KLIB_MEMBER(3, cm, AbstractMovementBase_speedOrders, OFF_CMOV_SPEED_MODE)) != MOVESPEED_GROUPED)
				{ allGrouped = false; break; }
			}
			if (allGrouped)
			{
				// The route planner's merge names the member the group leads with; only the formation's
				// member order changes, every per-character record above keeps the selection order.
				if (lead > 0 && lead < collectedCount)
				{
					uintptr_t first = collectedChars[0];
					collectedChars[0] = collectedChars[lead];
					collectedChars[lead] = first;
				}
				CreateFormationGroup(location, collectedChars, collectedCount);
				formedGroup = true;
			}
		}
		// A single-unit move, or a multi-unit move not all in "together" mode,
		// does not (re-)form a group for these characters: drop any group
		// membership they still carry from an earlier order.
		if (!formedGroup && collectedCount > 0)
			FormationDetachCharacters(collectedChars, collectedCount);
	}
	else if (task != 29
	)
	{
		// (a) Any non-move player order
		// (attack, job, pick-up, talk, ...) drops the IslandOrder of every
		// selected character, the same way IslandNoteOrder registers one for
		// task 29 above. An IslandOrder otherwise only clears on arrival,
		// squad removal, a save load or a new MOVE order, so a later non-move
		// order (or AI combat / a knockout replacing the order underneath the
		// player) left a stale move destination active for the stopped-park
		// test (item (e)) to eventually re-issue to a character the player
		// deliberately redirected. Same selected-characters walk as the task
		// 29 branch above.
		DropSelectedCharacterOrders(thisPI);
	}

	game::g_hookOrig.orig_addOrderSelected(thisPI, destIndoors, task, subject, shift, addDontClear, location);

	// Immediate reprio: the player just named one or more route zones, so the
	// jobs already queued for them float to the top now. Requests the AI task
	// system makes later ride the backstop instead.
	if (navmesh::g_navmeshCfg.reprioFastEnabled && charsAdded > 0)
	{
		CallPrioritizeNavMeshQueue();
		InterlockedIncrement(&reprioOrderFires);
	}
}
