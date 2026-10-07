#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/physx/hull_same_skip.h"

#ifdef KEO_DEBUG

#include "fixes/physx/hull_same_skip_policy.h"
#include "fixes/fixes_config.h"
#include "game/game.h"
#include "base/core.h"
#include <windows.h>
#include <cstdio>
#include <string>

// One writer, the physics thread: the table and s_clearSeen are touched only
// by the detour, after the install's init. Zero-initialised storage, no
// constructor.
static HullSameEntry s_slots[HULL_SAME_SLOTS];
static HullSameTable s_table;
static LONG s_clearSeen = 0;

typedef __int64 (*HullApply_t)(void*);
static HullApply_t s_origApply = NULL;   // the slot's thunk, read once at install

// Published by the main-thread tick; s_clearReq is raised before a switch to
// on is published, so the detour clears before its first decision in on.
static volatile LONG s_mode = 0;
static volatile LONG s_clearReq = 0;

static volatile LONG s_skipped = 0;
static volatile LONG s_teleports = 0;
static volatile LONG s_creates = 0;
static volatile LONG s_moves = 0;
static volatile LONG s_resets = 0;

// Main thread only.
static int s_seenMode = 0;
static bool s_wasLoading = false;
static double s_lastBeat = 0.0;

static const double kBeatSeconds = 60.0;

// Physics thread. Off: one read and the forward.
static __int64 HullApplyDetour(void* hull)
{
	if (!s_mode)
		return s_origApply(hull);

	const LONG req = s_clearReq;
	if (req != s_clearSeen)
	{
		HullSameClear(&s_table);
		s_clearSeen = req;
		InterlockedIncrement(&s_resets);
	}

	uintptr_t actor;
	bool teleport;
	unsigned pos[3];
	HullSameRead(hull, &actor, &teleport, pos);

	bool emptied = false;
	const HullApplyAction action = HullSameStep(&s_table, (uintptr_t)hull, actor, teleport, pos, &emptied);
	if (emptied)
		InterlockedIncrement(&s_resets);

	switch (action)
	{
	case HULL_SKIP:
		InterlockedIncrement(&s_skipped);
		return 0;
	case HULL_FORWARD_CREATE:
		InterlockedIncrement(&s_creates);
		break;
	case HULL_FORWARD_TELEPORT:
		InterlockedIncrement(&s_teleports);
		break;
	default:
		InterlockedIncrement(&s_moves);
		break;
	}
	return s_origApply(hull);
}

static LONG ReadCounter(volatile LONG* c)
{
	return InterlockedCompareExchange(c, 0, 0);
}

static void EmitHeartbeat()
{
	const LONG skipped = ReadCounter(&s_skipped);
	const LONG teleports = ReadCounter(&s_teleports);
	const LONG creates = ReadCounter(&s_creates);
	const LONG moves = ReadCounter(&s_moves);
	const LONG resets = ReadCounter(&s_resets);
	char line[256];
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"HullSame: mode=%s applies=%ld skipped=%ld teleports=%ld creates=%ld resets=%ld",
		s_seenMode ? "on" : "off", (long)(skipped + teleports + creates + moves),
		(long)skipped, (long)teleports, (long)creates, (long)resets);
	LogMsg(line);
}

void InstallHullSameSkip(int* installed, int*)
{
	(void)installed;
	HullSameInit(&s_table, s_slots, HULL_SAME_SLOTS);

	void** slot = (void**)GameAddr(RVA_PHYSICS_HULL_APPLY_SLOT);
	void* thunk = GameAddr(RVA_PHYSICS_HULL_APPLY_THUNK);
	const char* why = NULL;
	DWORD old = 0;
	if (*slot != thunk)
		why = "slot";
	else if (!HullSameThunkOk((const unsigned char*)thunk, (uintptr_t)thunk,
	                          (uintptr_t)GameAddr(RVA_PHYSICS_HULL_APPLY)))
		why = "thunk";
	else if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
		why = "protect";

	if (why)
	{
		LogMsg(std::string("HullSame: install=refused(") + why + ")");
		return;
	}

	// One aligned eight-byte store: the physics thread reads either pointer.
	s_origApply = (HullApply_t)*slot;
	InterlockedExchangePointer(slot, (void*)HullApplyDetour);
	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void*), old, &ignored);

	char line[64];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "HullSame: install=ok slot=0x%x",
		(unsigned)((uintptr_t)slot - gameBase));
	LogMsg(line);
}

void HullSameSkipTick(double now, bool saveLoading)
{
	const int mode = fixes::g_fixesCfg.cfg_hullSameSkip ? 1 : 0;
	bool beat = false;
	if (mode != s_seenMode)
	{
		if (mode)
			InterlockedIncrement(&s_clearReq);
		InterlockedExchange(&s_mode, mode);
		s_seenMode = mode;
		beat = true;
	}
	if (saveLoading && !s_wasLoading)
		InterlockedIncrement(&s_clearReq);
	s_wasLoading = saveLoading;

	if (beat || now - s_lastBeat >= kBeatSeconds)
	{
		s_lastBeat = now;
		EmitHeartbeat();
	}
}

#else  // !KEO_DEBUG

void InstallHullSameSkip(int* installed, int*) { (void)installed; }
void HullSameSkipTick(double now, bool saveLoading) { (void)now; (void)saveLoading; }

#endif // KEO_DEBUG
