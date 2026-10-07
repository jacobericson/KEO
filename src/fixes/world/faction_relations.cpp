// faction_relations.cpp - The faction relations switches: the entry detour on FactionRelations::update
// (relationsSelfFind), the pre-call detours on both affectRelations overloads (factionSelfGuard), the
// install step and the main-thread tick that hands the keys to them and writes the Relations: line.
// The detours run on the AI back thread or the main thread: one volatile read and a forward when
// off, Interlocked counters only, no lock, no allocation, no logging.
#include "fixes/world/faction_relations.h"

#ifdef KEO_DEBUG

#include "fixes/world/faction_relations_policy.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include "fixes/fixes_config.h"
#include <sstream>
#include <string>

typedef void (*relationsUpdate_t)(void* rel);
typedef void (*affectRelationsEvent_t)(void* rel, void* from, int ev, float mult);
typedef void (*affectRelationsAmount_t)(void* rel, void* from, float amount, float mult);

static relationsUpdate_t       orig_update = NULL;
static affectRelationsEvent_t  orig_event  = NULL;
static affectRelationsAmount_t orig_amount = NULL;

// Published by the tick, read by the detours.
static volatile LONG s_mode = 0;
static volatile LONG s_guard = 0;
// Latched by the update detour on any disagreement or refused table: the walk for the session.
static volatile LONG s_fallback = 0;
// Calls left in the verified stretch after a switch to on.
static volatile LONG s_verifyLeft = 0;

static volatile LONG s_found = 0;
static volatile LONG s_absent = 0;
static volatile LONG s_forwarded = 0;
static volatile LONG s_verifyBad = 0;
static volatile LONG s_hashBad = 0;
static volatile LONG s_selfBlocked = 0;
static volatile LONG s_selfMain = 0;

// Main thread only.
static int    s_seenMode = 0;
static int    s_seenGuard = 0;
static double s_lastBeat = 0;
static const char* s_updateWhy = "not run";
static const char* s_eventWhy = "not run";
static const char* s_amountWhy = "not run";

static void RelLatchForward(void* rel)
{
	InterlockedExchange(&s_fallback, 1);
	InterlockedIncrement(&s_forwarded);
	orig_update(rel);
}

static void hook_relationsUpdate(void* rel)
{
	const int mode = (int)s_mode;
	uintptr_t me = 0;
	bool fallback = false;
	bool inWindow = false;
	if (mode != REL_MODE_OFF)
	{
		me = *(const uintptr_t*)((const char*)rel + REL_OBJ_ME);
		fallback = s_fallback != 0;
		// The positive test keeps the count from wrapping back into the window.
		inWindow = mode == REL_MODE_ON && !fallback && me && s_verifyLeft > 0
		        && InterlockedDecrement(&s_verifyLeft) >= 0;
	}

	const RelPath path = RelChoosePath(mode, fallback, me, inWindow);
	if (path == REL_PATH_OFF)
	{
		orig_update(rel);
		return;
	}
	if (path == REL_PATH_FORWARD)
	{
		InterlockedIncrement(&s_forwarded);
		orig_update(rel);
		return;
	}

	const char* obj = (const char*)rel;
	RelTable t;
	t.buckets = *(const uintptr_t*)(obj + REL_OBJ_BUCKETS);
	t.bucketCount = *(const size_t*)(obj + REL_OBJ_BUCKET_COUNT);
	t.size = *(const size_t*)(obj + REL_OBJ_SIZE);

	uintptr_t node = 0;
	const RelFindResult found = RelLookup(t, me, &node);
	if (found == REL_LIMIT)
	{
		RelLatchForward(rel);
		return;
	}

	if (path == REL_PATH_VERIFY)
	{
		uintptr_t walk = 0;
		if (RelWalkFind(t, me, &walk) == REL_LIMIT)
		{
			RelLatchForward(rel);
			return;
		}
		const unsigned long long stored = walk ? *(const unsigned long long*)(walk + REL_NODE_HASH) : 0;
		const RelVerify v = RelVerifyLookup(node, walk, stored, RelKeyHash((unsigned long long)me));
		if (v == REL_VERIFY_NODE)
		{
			InterlockedIncrement(&s_verifyBad);
			InterlockedExchange(&s_fallback, 1);
		}
		else if (v == REL_VERIFY_HASH)
		{
			InterlockedIncrement(&s_hashBad);
			InterlockedExchange(&s_fallback, 1);
		}
		InterlockedIncrement(found == REL_FOUND ? &s_found : &s_absent);
		orig_update(rel);
		return;
	}

	if (found == REL_FOUND)
	{
		RelWriteSelf(node);
		InterlockedIncrement(&s_found);
	}
	else
	{
		InterlockedIncrement(&s_absent);
	}
}

// True when the guard drops this call; counts it, and its main-thread share.
static bool RelGuardTake(void* rel, void* from)
{
	if (!s_guard)
		return false;
	const uintptr_t me = *(const uintptr_t*)((const char*)rel + REL_OBJ_ME);
	if (!RelGuardDrops(1, me, (uintptr_t)from))
		return false;
	InterlockedIncrement(&s_selfBlocked);
	if (IsMainThread())
		InterlockedIncrement(&s_selfMain);
	return true;
}

static void hook_affectRelationsEvent(void* rel, void* from, int ev, float mult)
{
	if (RelGuardTake(rel, from))
		return;
	orig_event(rel, from, ev, mult);
}

static void hook_affectRelationsAmount(void* rel, void* from, float amount, float mult)
{
	if (RelGuardTake(rel, from))
		return;
	orig_amount(rel, from, amount, mult);
}

static void RelInstallToken(std::ostringstream& ss, const char* name, const char* why)
{
	ss << " " << name << "=";
	if (why)
		ss << "refused(" << why << ")";
	else
		ss << "ok";
}

void InstallFactionRelations(int* installed, int*)
{
	s_updateWhy = HookInstall(HOOK_FACTION_RELATIONS_UPDATE, hook_relationsUpdate, &orig_update, installed, true);
	s_eventWhy = HookInstall(HOOK_AFFECT_RELATIONS_EVENT, hook_affectRelationsEvent, &orig_event, installed, true);
	s_amountWhy = HookInstall(HOOK_AFFECT_RELATIONS_AMOUNT, hook_affectRelationsAmount, &orig_amount, installed,
	                          true);

	std::ostringstream ss;
	ss << "Relations: install";
	RelInstallToken(ss, "update", s_updateWhy);
	RelInstallToken(ss, "event", s_eventWhy);
	RelInstallToken(ss, "amount", s_amountWhy);
	if (s_updateWhy || s_eventWhy || s_amountWhy)
		ErrorLog(ss.str());
	else
		LogMsg(ss.str());
}

static LONG RelRead(volatile LONG* x)
{
	return InterlockedCompareExchange(x, 0, 0);
}

static const char* RelModeName(int mode)
{
	if (mode == REL_MODE_OFF)
		return "off";
	if (mode == REL_MODE_VERIFY)
		return "verify";
	return "on";
}

static void RelHeartbeat(double now)
{
	const LONG found = RelRead(&s_found);
	const LONG absent = RelRead(&s_absent);
	const LONG forwarded = RelRead(&s_forwarded);
	std::ostringstream ss;
	ss << "Relations: mode=" << RelModeName(s_seenMode);
	if (s_updateWhy)
		ss << " install=refused(update:" << s_updateWhy << ")";
	else if (s_eventWhy)
		ss << " install=refused(event:" << s_eventWhy << ")";
	else if (s_amountWhy)
		ss << " install=refused(amount:" << s_amountWhy << ")";
	else
		ss << " install=ok";
	ss << " calls=" << (long long)found + (long long)absent + (long long)forwarded
	   << " found=" << found << " absent=" << absent
	   << " verifyBad=" << RelRead(&s_verifyBad) << " hashBad=" << RelRead(&s_hashBad)
	   << " fallback=" << (RelRead(&s_fallback) ? 1 : 0)
	   << " guard=" << (s_seenGuard ? "on" : "off")
	   << " selfBlocked=" << RelRead(&s_selfBlocked) << " selfMain=" << RelRead(&s_selfMain);
	LogMsg(ss.str());
	s_lastBeat = now;
}

void FactionRelationsTick(double now)
{
	const int mode = fixes::g_fixesCfg.cfg_relationsSelfFind;
	const int guard = fixes::g_fixesCfg.cfg_factionSelfGuard;
	if (mode != s_seenMode || guard != s_seenGuard)
	{
		if (RelArmVerifyWindow(s_seenMode, mode))
			InterlockedExchange(&s_verifyLeft, 512);
		InterlockedExchange(&s_mode, (LONG)mode);
		InterlockedExchange(&s_guard, (LONG)guard);
		s_seenMode = mode;
		s_seenGuard = guard;
		RelHeartbeat(now);
		return;
	}
	if (now - s_lastBeat >= 60.0)
		RelHeartbeat(now);
}

#else  // !KEO_DEBUG

void InstallFactionRelations(int* installed, int*) { (void)installed; }
void FactionRelationsTick(double now) { (void)now; }

#endif // KEO_DEBUG
