#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/nest_validation.h"
#include "fixes/world/nest_validation_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include <string>
#include <cstring>
#include "base/klib_include.h"
#include <core/Functions.h>
#include "base/klib_include_end.h"

static finalizeZoneResources_t orig_finalizeZoneResources_guard = NULL;

static volatile LONG s_skippedCount     = 0;
static volatile LONG s_revalidatedCount = 0;
static volatile LONG s_destroyedCount   = 0;

// One flag per AreaSector cell (64x64, row-major, matching the record's own
// (x, y)). Main thread only -- the sole caller runs there -- so a plain
// array needs no lock.
static unsigned char s_skipped[64 * 64] = { 0 };

// TownList::destroy: called from other places besides finalizeZoneResources
// (Town::reassessTownPosition, the world reset, mergePlayerTowns, and more --
// two of its six callers are vtable-dispatched and not exhaustively traceable
// through the slot), so a call only counts as a nest destroy this guard let
// through while it runs inside CallOriginalTracked's own call to the
// original below. __declspec(thread) rather than resting on a "main thread
// only" argument that two of those callers cannot fully support: a call on
// another thread then simply never sees the flag set, at no extra cost.
typedef void (*townListDestroy_t)(void* townList, void* town);
static townListDestroy_t orig_townListDestroy = NULL;
static __declspec(thread) bool t_insideGuardedOrig = false;

static void hook_townListDestroy(void* townList, void* town)
{
	if (t_insideGuardedOrig)
		InterlockedIncrement(&s_destroyedCount);
	orig_townListDestroy(townList, town);
}

// Every call to the original finalizeZoneResources goes through here so the
// destroyed counter attributes correctly regardless of which path led to it
// (a decoded skip that proceeded, or a record/section-manager fallback that
// never consulted the skip logic at all). __finally guarantees the flag is
// restored even if the original throws or unwinds through here (this
// codebase already recovers from exactly that kind of unwind elsewhere,
// e.g. navmesh_update_guard.cpp): without it, an unwound call would leave
// the flag stuck true and attribute every later TownList::destroy on this
// thread to this guard for the rest of the session.
static void CallOriginalTracked(void* sectionEntry)
{
	bool prevInside = t_insideGuardedOrig;
	t_insideGuardedOrig = true;
	__try
	{
		orig_finalizeZoneResources_guard(sectionEntry);
	}
	__finally
	{
		t_insideGuardedOrig = prevInside;
	}
}

static void hook_finalizeZoneResources(void* sectionEntry)
{
	if (!fixes::g_fixesCfg.nestValidationGuardEnabled || !sectionEntry)
	{
		CallOriginalTracked(sectionEntry);
		return;
	}

	const unsigned int* rec = (const unsigned int*)sectionEntry;
	int cellIndex = 0;
	if (!NestValidationCellFromRecord(rec[0], rec[1], &cellIndex))
	{
		// The record's leading fields did not decode to a grid cell: nothing
		// to check against, so run the original exactly as before the guard.
		CallOriginalTracked(sectionEntry);
		return;
	}

	uintptr_t sectionMgr = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_SECTION_MGR));
	bool meshReady = true;  // no section manager or readiness function: fail open, run as before
	if (sectionMgr && game::g_hookOrig.orig_isContentPending)
	{
		int gridCoords[2] = { (int)rec[0], (int)rec[1] };
		meshReady = game::g_hookOrig.orig_isContentPending((void*)sectionMgr, (void*)gridCoords);
	}

	NestValidationDecision d = NestValidationDecide(meshReady, s_skipped[cellIndex] != 0);
	if (d.action == NESTVAL_SKIP)
	{
		s_skipped[cellIndex] = 1;
		if (d.countSkip)
			InterlockedIncrement(&s_skippedCount);
		return;  // never destroy a nest against a mesh that is not in
	}

	if (d.countRevalidated)
	{
		s_skipped[cellIndex] = 0;
		InterlockedIncrement(&s_revalidatedCount);
	}
	CallOriginalTracked(sectionEntry);
}

void InstallNestValidationGuard(int* installed, int*)
{
#if ZONEHAND_STEP >= 2
	if (!HookRowWanted(HOOK_FINALIZE_ZONE_RES_ENTRY))
		return;

	const char* why = HookInstall(HOOK_FINALIZE_ZONE_RES_ENTRY, hook_finalizeZoneResources,
			&orig_finalizeZoneResources_guard, installed, true);

	if (!why)
	{
		LogMsg("Nest validation guard: installed");
	}
	else
	{
		orig_finalizeZoneResources_guard = NULL;
		LogError(std::string("Nest validation guard: not installed (") + why + ")");
	}

	// The destroyed counter is independent of the guard above: it installs
	// even if the guard failed to (it would then just never see the flag
	// set, and count nothing -- correct, since nothing here is attributable).
	const char* destroyWhy = HookInstall(HOOK_TOWNLIST_DESTROY, hook_townListDestroy,
			&orig_townListDestroy, installed, true);

	if (!destroyWhy)
	{
		LogMsg("Nest validation guard: destroyed counter installed");
	}
	else
	{
		orig_townListDestroy = NULL;
		LogError(std::string("Nest validation guard: destroyed counter not installed (") + destroyWhy + ")");
	}
#else
	// The two rows exist from step 2; below it the guard has nothing to install.
	(void)installed;
	orig_finalizeZoneResources_guard = NULL;
	orig_townListDestroy = NULL;
	LogError("Nest validation guard: not installed (no manifest row below ZONEHAND_STEP 2)");
#endif
}

long NestValidationSkippedCount()     { return InterlockedCompareExchange(&s_skippedCount, 0, 0); }
long NestValidationRevalidatedCount() { return InterlockedCompareExchange(&s_revalidatedCount, 0, 0); }
long NestValidationDestroyedCount()   { return InterlockedCompareExchange(&s_destroyedCount, 0, 0); }

void NestValidationClearOnReset()
{
	memset(s_skipped, 0, sizeof(s_skipped));
}
