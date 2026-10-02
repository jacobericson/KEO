#include "zone/preload/preload_prepare.h"
#include "zone/transition.h"
#include "navmesh/nm_workers.h"
#include "navmesh/generation/nm_misspar.h"
#include "movement/tracking.h"
#include "movement/formation.h"
#include "movement/islands.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "zone/preload/zone_cycle_stats.h"
#include "zone/preload/preload_internal.h"
#include "zone/handoff/zone_handoff.h"
#include "zone/handoff/zone_adoption_seam.h"
#include "zone/geometry/zone_geometry_epoch.h"
#include <cstdio>     // _snprintf_s (hook_resetUnloadZones builds its line without CRT streams)
#include <psapi.h>    // PROCESS_MEMORY_COUNTERS_EX only; the function is resolved at runtime


// Calls content->vtable[4](content) — the game's processContent function.
// Populates things, objects, and buildings from the zone data file.
// Same call that processState2 makes, but invoked directly.
static void CallProcessContent(void* content)
{
	// A geometry boundary this mod owns, bracketed here rather than at each
	// call site so no future caller can miss it: the call instantiates the
	// cell's saved objects, and their collision follows. A generation whose
	// read spans this call cannot know whether it saw the cell before or
	// after it.
	ZoneGeometryMutationScope geomScope;
	KlibProcessZoneContent(content);
}

void TryRegisterPreloadedZones(void* zoneMgr, double now)
{
	int zmState = GetZoneState(zoneMgr);
	if (zmState != 0)
		return;

	// isReadyForSections: 4 work queues empty + _queuesClear (physics+0x320) set
	uintptr_t physics = *(uintptr_t*)((uintptr_t)GameAddr(RVA_PAUSESTATE_PHYSICS));
	if (!physics)
		return;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToMake_mainThreadData_count, 432)) > 0) return;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_actorsToDestroy_mainThreadData_count, 680)) > 0) return;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_terrainToLoad_mainThreadData_count, 760)) > 0) return;
	if (*(int*)(KLIB_MEMBER(2, physics, PhysicsInterface_hullsToDestroy_mainThreadData_count, 512)) > 0) return;
	if (*(unsigned char*)(KLIB_MEMBER(2, physics, PhysicsInterface__queuesClear, 800)) == 0) return;

	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	if (!sectionMgr)
		return;
	if (!game::g_gameFn.fn_registerZoneSections)
		return;

	int order[MAX_PRELOADED];
	int orderCount = BuildPreloadOrder(order);

	for (int k = 0; k < orderCount; ++k)
	{
		int i = order[k];
		if (!preloadedZones[i].pending)
			continue;
		if (preloadedZones[i].registered)
			continue;
		if (preloadedZones[i].gameOwned)
			continue;

		void* ze = preloadedZones[i].zoneEntry;
		if (!ze)
			continue;

		if (!IsZoneLoading(ze) || IsZoneAccessible(ze))
			continue;

		void* content = *(void**)ze;
		if (!content)
			continue;

		// Poll isZoneReady and content+264 every frame for every
		// pending, unregistered zone examined here -- cheap and non-blocking,
		// independent of the age gates below, which decide only when
		// CallProcessContent is actually called.
		if (preloadedZones[i].pipeLoadSec >= 0.0)
		{
			if (!preloadedZones[i].pipeIsReadySeen && fn_isZoneReady && fn_isZoneReady(ze))
			{
				preloadedZones[i].pipeIsReadySeen = true;
				PipeSampleAdd(g_pipe.toReadyMs, &g_pipe.toReadyN,
				              (now - preloadedZones[i].pipeLoadSec) * 1000.0);
			}
			if (!preloadedZones[i].pipe264Seen &&
			    *(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG)) == 0)
			{
				preloadedZones[i].pipe264Seen = true;
				PipeSampleAdd(g_pipe.to264Ms, &g_pipe.to264N,
				              (now - preloadedZones[i].pipeLoadSec) * 1000.0);
			}
		}

		int thingsCount = *(int*)(KLIB_MEMBER(2, (uintptr_t)content, RootObjectContainer_things_count, OFF_ZMC_THINGS_COUNT));

		// processContent (vtable[4]): populates things/objects from zone data.
		// Called once per zone, after content streaming has had time to finish.
		// things<=1 means no real content yet (0=unprocessed, 1=processed-empty).
		if (thingsCount <= 1 && !preloadedZones[i].contentProcessed)
		{
#if ZONEHAND_STEP >= 2
			// Call the finalize exactly when the engine's own tick would:
			// the terrain record is in and the content still carries its
			// activation flag. A fixed wait is a guess in both directions,
			// too long for a warm cell and too short for a cold one.
			bool terrainIn = fn_isZoneReady && fn_isZoneReady(ze);
			bool flagSet = *(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG)) != 0;
			if (!ZoneContentNeedsFinalize(terrainIn, flagSet))
				continue;
#else
			double age = now - preloadedZones[i].loadTimeSec;
			if (age < 2.0)
				continue;  // give content streaming time to finish
#endif

			// Registry guard: processContent instantiates the zone's
			// saved objects, and each resolves its handle through this slot.
			if (!RegistryGuardPasses(ze, REG_SITE_PROCESS))
			{
				DropTrackedZone(i);
				continue;
			}

			// The handoff rule (item 2): decided here, before the finalize,
			// since content+0xA8 means nothing until it runs. A zone the game
			// will read as first-time (or a town cell) is given back
			// untouched, so the game's own load runs phase B for it.
			if (ZlFirstTimeRuleOn() && ZlPredictFirstTime(preloadedZones[i].gridX, preloadedZones[i].gridY) != 0)
			{
				ZlGiveToGame(zoneMgr, i, ze, content, "at the processContent decision");
				continue;
			}

			// isZoneReady's state right at the moment the age gate fires the
			// call -- answers "notReadyAtCall" below.
			bool wasReadyAtCall = preloadedZones[i].pipeIsReadySeen;
			LARGE_INTEGER pcStart, pcEnd;
			QueryPerformanceCounter(&pcStart);

			CallProcessContent(content);
			preloadedZones[i].contentProcessed = true;
			ZoneHandoffNoteContentInitialized(preloadedZones[i].gridX, preloadedZones[i].gridY);
			ZlTouchZone(ze, ZL_PROCESSED);
			thingsCount = *(int*)(KLIB_MEMBER(2, (uintptr_t)content, RootObjectContainer_things_count, OFF_ZMC_THINGS_COUNT));

			QueryPerformanceCounter(&pcEnd);
			PipeSampleAdd(g_pipe.procContentMs, &g_pipe.procContentN, QPCToMs(pcStart, pcEnd));

			bool clear264After = (*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG)) == 0);
			if (!preloadedZones[i].pipe264Seen && clear264After && preloadedZones[i].pipeLoadSec >= 0.0)
			{
				preloadedZones[i].pipe264Seen = true;
				PipeSampleAdd(g_pipe.to264Ms, &g_pipe.to264N,
				              (now - preloadedZones[i].pipeLoadSec) * 1000.0);
			}

			if (thingsCount == 0)      g_pipe.thingsAfter0++;
			else if (thingsCount == 1) g_pipe.thingsAfter1++;

			// Does isZoneReady pass while things==0 after finalizeContent?
			// readyEmpty answers "yes, and it still came back empty";
			// notReadyAtCall counts calls the age gate fired despite
			// isZoneReady still being false.
			if (wasReadyAtCall && clear264After && thingsCount <= 1)
				g_pipe.readyEmpty++;
			if (!wasReadyAtCall)
				g_pipe.notReadyAtCall++;
		}

#ifdef KEO_DEBUG
		ZlLogFirstTimeDiag(i, content);   // FirstTime: line, once per cell
#endif
		// The handoff rule, after the finalize: every zone the mod registers
		// is one the game would not populate as first-time,
		// and is saveable if the step-3 pass unloads it. Two kinds are not and
		// never go further:
		//   - loaded == 0: processContent ran before the terrain collision
		//     loaded (ZoneMapContent::update finalizes only then), so this is
		//     a shell -- its first-time byte is still the ctor's, and saving
		//     it later would write an item list over no buildings. Finalized
		//     once the terrain is in, within the stall budget;
		//   - first-time after all (the "imported" key or an unreadable zone
		//     file, which the prediction cannot see): the game's, below --
		//     except where the unload protocol can never pass, where the mod
		//     keeps it (ZlKeepLate: no zombie handoff, so no
		//     +176 clear on finalized content).
		if (ZlFirstTimeRuleOn())
		{
			int ft = 0, ld = 0, act = 0;
			bool readOk = ReadContentLifeFlags(content, &ft, &ld, &act);
			if (readOk && ld == 0)
			{
				if (preloadedZones[i].contentProcessed && fn_isZoneReady && fn_isZoneReady(ze))
					CallProcessContent(content);
				continue;
			}
			if (!readOk || ft != 0)
			{
				if (ZlLateHandoffPossible())
				{
					ZlGiveToGame(zoneMgr, i, ze, content, "read first-time after the finalize");
					continue;
				}
				ZlKeepLate(i);   // and on to registration as usual
			}
		}

		// Empty zones: things<=1 after processContent means no real content.
		// things=0: no data file processed. things=1: data file processed, nothing to load.
		if (thingsCount <= 1)
		{
#if ZONEHAND_STEP >= 2
			// Here the question is the other one: only once the finalize has
			// run is "nothing in it" an answer rather than a zone whose data
			// has yet to arrive.
			if (!fn_isZoneReady || !fn_isZoneReady(ze))
				continue;
			if (!ZoneContentIsFinalized(*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)content, ZoneMapContent_activationFlag, OFF_ZMC_READY_FLAG)) != 0))
				continue;
#else
			double age = now - preloadedZones[i].loadTimeSec;
			if (age < 3.0)
				continue;
#endif
		}

		// Registry guard: never hand the game's section registration
		// a zone whose content is not the one registered for it.
		if (!RegistryGuardPasses(ze, REG_SITE_REGISTER))
		{
			DropTrackedZone(i);
			continue;
		}

#if ZONEHAND_STEP >= 2
		// The entry check ran before this pass instantiated anything. A
		// processContent call in the same pass queues its objects' hulls, so
		// the counts registration depends on are re-read here rather than
		// inherited from the top of the function.
		if (!ZoneHandoffPhysicsCountsClear())
			continue;
#endif

		game::g_gameFn.fn_registerZoneSections((void*)sectionMgr, ze);
		// Post-condition: every game caller of registerZoneSections clears
		// ZoneMap+0xCC afterwards, and each writes a single byte. Writing an
		// int here would zero the three bytes that follow it.
		*(unsigned char*)(KLIB_MEMBER(2, (uintptr_t)ze, ZoneMap__generateNavMeshesFlag, 204)) = 0;

		preloadedZones[i].registered = true;
		preloadedZones[i].registeredEmpty = (thingsCount <= 1);
		ZoneHandoffNoteRegistered(preloadedZones[i].gridX, preloadedZones[i].gridY);
		ZlTouchZone(ze, ZL_REGISTERED);

		g_pipe.registered++;
		if (preloadedZones[i].pipeLoadSec >= 0.0)
			PipeSampleAdd(g_pipe.toRegMs, &g_pipe.toRegN,
			              (now - preloadedZones[i].pipeLoadSec) * 1000.0);

		preloadedZones[i].loadTimeSec = now;  // reset: age since registration (stall timeout, zombie age)

		{
			std::ostringstream ss;
			ss << "Registered zone ("
			   << preloadedZones[i].gridX << ","
			   << preloadedZones[i].gridY << ") things="
			   << thingsCount
			   << (preloadedZones[i].registeredEmpty ? " (empty)" : "");
			LogMsg(ss.str());
		}

		return;  // one per frame
	}
}
