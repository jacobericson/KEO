// faction_relations.cpp - Faction self-relation lookup and its verified fallback.
#include "fixes/world/faction_relations.h"

#include "fixes/world/faction_relations_policy.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include "fixes/fixes_config.h"
#include <sstream>
#include <string>

typedef void (*relationsUpdate_t)(void* rel);

static relationsUpdate_t       orig_update = NULL;

// Published by the tick, read by the detours.
static volatile LONG s_mode = 0;
// Latched by the update detour on any disagreement or refused table: the walk for the session.
static volatile LONG s_fallback = 0;
// Calls left in the verified stretch after a switch to on.
static volatile LONG s_verifyLeft = 0;

#ifdef KEO_DEBUG
static volatile LONG s_found = 0;
static volatile LONG s_absent = 0;
static volatile LONG s_forwarded = 0;
static volatile LONG s_verifyBad = 0;
static volatile LONG s_hashBad = 0;
#endif

// Main thread only.
static int    s_seenMode = 0;
#ifdef KEO_DEBUG
static double s_lastBeat = 0;
#endif
static const char* s_updateWhy = "not run";

static void RelLatchForward(void* rel)
{
	InterlockedExchange(&s_fallback, 1);
#ifdef KEO_DEBUG
	InterlockedIncrement(&s_forwarded);
#endif
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
#ifdef KEO_DEBUG
		InterlockedIncrement(&s_forwarded);
#endif
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
#ifdef KEO_DEBUG
			InterlockedIncrement(&s_verifyBad);
#endif
			InterlockedExchange(&s_fallback, 1);
		}
		else if (v == REL_VERIFY_HASH)
		{
#ifdef KEO_DEBUG
			InterlockedIncrement(&s_hashBad);
#endif
			InterlockedExchange(&s_fallback, 1);
		}
#ifdef KEO_DEBUG
		InterlockedIncrement(found == REL_FOUND ? &s_found : &s_absent);
#endif
		orig_update(rel);
		return;
	}

	if (found == REL_FOUND)
	{
		RelWriteSelf(node);
#ifdef KEO_DEBUG
		InterlockedIncrement(&s_found);
#endif
	}
#ifdef KEO_DEBUG
	else
	{
		InterlockedIncrement(&s_absent);
	}
#endif
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

	std::ostringstream ss;
	ss << "Relations: install";
	RelInstallToken(ss, "update", s_updateWhy);
	if (s_updateWhy)
		ErrorLog(ss.str());
	else
		LogMsg(ss.str());
}

#ifdef KEO_DEBUG
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
	else
		ss << " install=ok";
	ss << " calls=" << (long long)found + (long long)absent + (long long)forwarded
	   << " found=" << found << " absent=" << absent
	   << " verifyBad=" << RelRead(&s_verifyBad) << " hashBad=" << RelRead(&s_hashBad)
	   << " fallback=" << (RelRead(&s_fallback) ? 1 : 0);
	LogMsg(ss.str());
	s_lastBeat = now;
}
#endif

void FactionRelationsTick(double now)
{
	const int mode = fixes::g_fixesCfg.cfg_relationsSelfFind;
	if (mode != s_seenMode)
	{
		if (RelArmVerifyWindow(s_seenMode, mode))
			InterlockedExchange(&s_verifyLeft, 512);
		InterlockedExchange(&s_mode, (LONG)mode);
		s_seenMode = mode;
#ifdef KEO_DEBUG
		RelHeartbeat(now);
#endif
		return;
	}
#ifdef KEO_DEBUG
	if (now - s_lastBeat >= 60.0)
		RelHeartbeat(now);
#else
	(void)now;
#endif
}
