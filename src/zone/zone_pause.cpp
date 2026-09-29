#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "zone/zone_pause.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include "zone/preload/zone_cycle_stats.h"
#include <string.h>
#include <sstream>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/gui/ForgottenGUI.h>
#include "base/klib_include_end.h"

typedef bool (*processLoading_t)(void* zoneMgr);
static processLoading_t orig_processLoading = NULL;

static const volatile unsigned* s_menuGuard    = NULL;  // bit 0: the menu singleton exists
static void* const volatile*    s_menuInstance = NULL;
static long s_restored = 0;          // main thread only

// Absolute target of a RIP-relative load whose opcode bytes are `op` and whose
// disp32 follows them; NULL when the bytes differ.
static const void* RipLoadTarget(uintptr_t insn, const unsigned char* op, size_t opLen)
{
	if (memcmp((const void*)insn, op, opLen) != 0)
		return NULL;
	int disp = *(const int*)(insn + opLen);
	return (const void*)(insn + opLen + 4 + disp);
}

static bool EscapeMenuVisible()
{
	if ((*s_menuGuard & 1) == 0 || *s_menuInstance == NULL)
		return false;          // never opened: isPaused would construct it
	return gui->isPaused();
}

// The loader clears `paused` unconditionally at phase 3->4 and at completion;
// togglePause(false) keeps a pause only when frameSpeedMult is 0, which the
// escape menu's pause never sets. When that happens behind an open menu, take
// the pause back and let the menu's hide release it (its byte at +0xE8 set to 0
// means "I paused, so I unpause").
static bool hook_processLoading(void* zoneMgr)
{
	bool before = ou->paused;
	// The wedge guard is a separate flag from the cycle stats and is on where
	// the stats are off, so the sample runs for either. The phase either side
	// is what the wedge machine needs; an unread phase is not a sample and
	// would disable it. The call's own duration only feeds the stats totals,
	// but taking it either way keeps one set of totals rather than two.
	bool sample = (zone::g_zoneCfg.zoneCycleStatsEnabled || zone::g_zoneCfg.zoneWedgeGuardEnabled) && zoneMgr != NULL;
	int phaseBefore = sample ? GetZoneState(zoneMgr) : -1;
	LARGE_INTEGER t0, t1;
	t0.QuadPart = 0;
	t1.QuadPart = 0;
	if (sample)
		QueryPerformanceCounter(&t0);
	bool result = orig_processLoading(zoneMgr);
	if (sample)
		QueryPerformanceCounter(&t1);
	if (before && !ou->paused && EscapeMenuVisible())
	{
		ou->togglePause(true);
		*((unsigned char*)*s_menuInstance + OFF_ESC_MENU_WAS_PAUSED) = 0;
		// The loader takes and releases its own pause on alternate frames, so
		// a line per restore would flood: only the first is logged, and the
		// running count rides the per-cycle line.
		if (++s_restored == 1)
			LogMsg("Escape menu pause kept through a loader unpause (first; later restores are counted)");
		ZoneCyclePauseRestored();
	}
	// Each phase branch is tested before the one that feeds it, so a call
	// advances the phase at most once through the branches: the 3->4 unload
	// and the 4->5 publish happen inside this call, and its duration is that
	// edge's window. The tail can still write phase 1 on top of any of them.
	if (sample)
		ZoneCycleOnProcessLoading(zoneMgr, phaseBefore, GetZoneState(zoneMgr), QPCToMs(t0, t1));
	return result;
}

void InstallZonePauseGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_PROCESS_LOADING))
		return;

	static const unsigned char opGuard[] = { 0x8B, 0x05 };
	static const unsigned char opMenu[]  = { 0x48, 0x8B, 0x05 };
	static const unsigned char hideCmp[] = { 0x80, 0xB9, 0xE8, 0x00, 0x00, 0x00, 0x00 };
	uintptr_t isPaused = (uintptr_t)GameAddr(RVA_GUI_IS_PAUSED);

	const char* why = NULL;
	if ((uintptr_t)ou != gameBase + RVA_GLOBAL_GAMEWORLD || (uintptr_t)gui != gameBase + RVA_GLOBAL_GUI)
		why = "globals";
	else if ((const void*)KlibRealAddress(&GameWorld::togglePause) != GameAddr(RVA_GW_TOGGLE_PAUSE)
	      || (const void*)KlibRealAddress(&ForgottenGUI::isPaused) != GameAddr(RVA_GUI_IS_PAUSED))
		why = "addresses";
	else if (memcmp((const char*)GameAddr(RVA_ESC_MENU_HIDE) + 6, hideCmp, sizeof(hideCmp)) != 0)
		why = "menu hide bytes";
	else
	{
		s_menuGuard    = (const volatile unsigned*)RipLoadTarget(isPaused + OFF_GUI_ISPAUSED_GUARD_OP, opGuard, sizeof(opGuard));
		s_menuInstance = (void* const volatile*)RipLoadTarget(isPaused + OFF_GUI_ISPAUSED_MENU_OP, opMenu, sizeof(opMenu));
		if (!s_menuGuard || !s_menuInstance)
			why = "isPaused operands";
	}

	if (!why && HookInstall(HOOK_PROCESS_LOADING, hook_processLoading,
			&orig_processLoading, installed, true) == NULL)
	{
		LogMsg("Escape menu pause guard: installed");
		return;
	}
	orig_processLoading = NULL;
	ErrorLog(std::string("Escape menu pause guard: not installed (") + (why ? why : "prologue/hook") + ")");
}

bool ZonePauseIsPaused()
{
	return ou->paused;
}
