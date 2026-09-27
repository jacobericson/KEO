#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/section_key_probe.h"

#ifdef ZONEOPT_DEBUG
#include "fixes/streaming/section_key_ring.h"
#include "fixes/streaming/section_key_scan.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include <sstream>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// Two detours that read, and change nothing. Both run on the navmesh thread
// inside the world step, under the collection's change mutex: no lock, no
// allocation, no logging on the call path, and no branch whose outcome depends
// on what was read. The scan they call lives next door so it can be faulted
// deliberately without the game.
//
// The clearance reset walks a list of packed keys and indexes the instance
// array with each key's section half. Both of its key sources are readable at
// entry -- the caller's array and the context's own pending list -- so the set
// the loop will use is classified before the first indexed read happens. The
// cut lookup takes one key as an argument and makes the same read immediately,
// so one capture covers it.

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// Unconditional, on a timer. A probe that only spoke when it saw something
// wrong would make "saw nothing wrong" and "never ran" the same silence.
static void MaybeHeartbeat()
{
	if (!GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds))
		return;
	char line[256];
	SectionKeyRingHeartbeatLine(line, sizeof(line));
	LogMsgDeferrable(line);
}

typedef void* (*clearanceResetKeys_t)(void* ctx, void* a2, void* a3, void* keyArray);
typedef __int64 (*sectionCutLookup_t)(void* collection, unsigned int packedKey, void* a3);

static clearanceResetKeys_t orig_clearanceResetKeys = NULL;
static sectionCutLookup_t   orig_sectionCutLookup   = NULL;

static void* hook_clearanceResetKeys(void* ctx, void* a2, void* a3, void* keyArray)
{
	SectionKeyScanClearance(ctx, keyArray);
	// Emitted before the call that can fault, so a session that dies in it
	// still carries the totals.
	MaybeHeartbeat();
	return orig_clearanceResetKeys(ctx, a2, a3, keyArray);
}

static __int64 hook_sectionCutLookup(void* collection, unsigned int packedKey, void* a3)
{
	SectionKeyScanCut(collection, packedKey);
	return orig_sectionCutLookup(collection, packedKey, a3);
}

void InstallSectionKeyProbe(int* installed, int*)
{
	if (!HookRowWanted(HOOK_CLEARANCE_RESET_KEYS))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;
	SectionKeyRingInit();

	bool clearanceOk = false, cutOk = false;

	const char* why = HookInstallRow(HOOK_CLEARANCE_RESET_KEYS, hook_clearanceResetKeys,
			(void**)&orig_clearanceResetKeys, installed, true);
	if (!why)
	{
		clearanceOk = true;
	}
	else
	{
		orig_clearanceResetKeys = NULL;
		ErrorLog(std::string("Section key probe: the clearance-reset site is not hooked (")
		         + why + "); its key set is not recorded");
	}

	why = HookInstallRow(HOOK_SECTION_CUT_LOOKUP, hook_sectionCutLookup,
			(void**)&orig_sectionCutLookup, installed, true);
	if (!why)
	{
		cutOk = true;
	}
	else
	{
		orig_sectionCutLookup = NULL;
		ErrorLog(std::string("Section key probe: the cut-lookup site is not hooked (")
		         + why + "); its key is not recorded");
	}

	std::ostringstream ss;
	ss << "Section key probe: clearance=" << (clearanceOk ? 1 : 0)
	   << " cut=" << (cutOk ? 1 : 0)
	   << "; the crash record carries the last section-table lookups"
	   << " (a heartbeat line follows the first minute)";
	LogMsg(ss.str());
}

#else

// Nothing of this probe exists outside a DEV build; hook_manifest.cpp does not name it.
typedef int SectionKeyProbeNotInThisBuild;

#endif // ZONEOPT_DEBUG
