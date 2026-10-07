#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/world/town_claim.h"
#include "fixes/world/town_claim_policy.h"
#include "game/game.h"
#include "base/core.h"
#include "base/config.h"
#include "base/fixed_log_buf.h"
#include "plugin/hook_manifest.h"
#include "zone/handoff/first_time_bindings.h"
#include <windows.h>
#include <string>
#include "base/klib_include.h"
#include <kenshi/Town.h>            // TownList: things, noTown
#include <kenshi/Faction.h>         // Faction::isPlayer
#include <kenshi/RootObject.h>      // RootObjectBase::pos
#include "base/klib_include_end.h"

typedef void  (__fastcall *builderPlace_t)(void* preview);
typedef void* (__fastcall *createBuilding_t)(void* factory, void* data, const float* position, void* town,
	void* owner, const float* rotation, void* callbackObject, void* furnitureOf, void* isDoorOf,
	void* saveState, void* isIndoorsOf, bool invisible, bool completed, bool isFoliage,
	int floorNumber, bool isOutsideFurniture);
static builderPlace_t   orig_builderPlace   = NULL;
static createBuilding_t orig_createBuilding = NULL;
static __declspec(thread) int t_placeDepth    = 0;
static __declspec(thread) int t_townClaimSkip = 0;
static __declspec(thread) int t_gateAsked     = 0;   // the gate's calls on this thread
static bool s_rowsInstalled = false;
// flagged == pass + containing + nullTown; snapNpc counts the snap rule's share of the last two.
// nullList counts a noTown answer with no TownList: that call goes on unflagged with its own town.
static volatile LONG s_flagged = 0, s_pass = 0, s_containing = 0, s_nullTown = 0,
                     s_snapNpc = 0, s_recheckSkips = 0, s_candidateCap = 0, s_nullList = 0;
static const size_t kVtGetFaction = 0x58;    // RootObjectBase::getFaction
static const size_t kVtIsNest     = 0x270;   // TownBase::isNest
static const size_t kVtGetRadius  = 0x2A0;   // TownBase::getRadius

typedef void* (__fastcall *GetFactionFn)(void* self);
typedef void* (__fastcall *IsNestFn)(void* self);
typedef float (__fastcall *GetRadiusFn)(void* self);

namespace town_claim_detail {
// The builder's depth for one call, restored on every exit including a C++ unwind.
struct PlaceDepthScope
{
	PlaceDepthScope()  { ++t_placeDepth; }
	~PlaceDepthScope() { --t_placeDepth; }
};
// The gate's flag for one createBuilding call, restored on every exit including a C++ unwind.
struct SkipFlagScope
{
	int saved;
	explicit SkipFlagScope(int v) : saved(t_townClaimSkip) { t_townClaimSkip = v; }
	~SkipFlagScope() { t_townClaimSkip = saved; }
};
} // namespace town_claim_detail
using namespace town_claim_detail;

// Main thread: the builder places one previewed building through createBuilding.
static void __fastcall hook_builderPlace(void* preview)
{
	PlaceDepthScope s;
	orig_builderPlace(preview);
}

static bool FactionIsPlayer(void* f)
{
	return f && ((Faction*)f)->isPlayer != NULL;
}

// The object's own getFaction through its vtable; NULL for a NULL object.
static void* ObjectFaction(void* o)
{
	if (!o)
		return NULL;
	GetFactionFn fn = *(GetFactionFn*)(*(char**)o + kVtGetFaction);
	return fn(o);
}

// The snap target the builder hands over in a SetMountedBuildingCallback, else NULL.
static void* SnapTargetOf(void* cb)
{
	return cb && *(uintptr_t*)cb == (uintptr_t)GameAddr(RVA_SET_MOUNTED_CALLBACK_VTABLE)
	     ? *(void**)((char*)cb + 0x10) : NULL;
}

static TownList* Towns()
{
	return *(TownList**)GameAddr(RVA_GLOBAL_TOWN_LIST);
}

// The town's own isNest through its vtable: non-NULL for a nest.
static bool TownIsNest(void* t)
{
	IsNestFn fn = *(IsNestFn*)(*(char**)t + kVtIsNest);
	return fn(t) != NULL;
}

// Main thread. The owner's town whose radius covers the spot, the same centre (pos x and z),
// radius and exclusions (nest markers and nests) the game's own containing-town lookup uses;
// NULL when none does. The faction test runs first, so a town of another faction is never
// asked whether it is a nest.
static void* FindContainingPlayerTown(void* owner, const float* position)
{
	TownList* list = Towns();
	if (!list || !list->things.stuff)
		return NULL;

	TownClaimCandidate cand[kTownClaimMaxCandidates];
	void* towns[kTownClaimMaxCandidates];
	int n = 0;
	const unsigned int count = list->things.count;
	for (unsigned int i = 0; i < count; ++i)
	{
		RootObject* t = list->things.stuff[i];
		if (!t)
			continue;
		const bool faction = ObjectFaction(t) == owner;
		const bool marker = faction && ((TownBase*)t)->townType == TOWN_NEST_MARKER;
		const bool nest = faction && !marker && TownIsNest(t);
		if (!TownClaimTownEligible(faction, marker, nest))
			continue;
		if (n >= kTownClaimMaxCandidates)
		{
			InterlockedIncrement(&s_candidateCap);
			break;
		}
		const RootObjectBase* base = (const RootObjectBase*)t;
		GetRadiusFn radius = *(GetRadiusFn*)(*(char**)t + kVtGetRadius);
		cand[n].x = base->pos.x;
		cand[n].z = base->pos.z;
		cand[n].radius = radius(t);
		towns[n] = t;
		++n;
	}
	const int pick = TownClaimPickContaining(cand, n, position[0], position[2]);
	return pick >= 0 ? towns[pick] : NULL;
}

static void* NullTown()
{
	TownList* list = Towns();
	return list ? (void*)list->noTown : NULL;
}

static void CountDecision(TownClaimAction action, const TownClaimInputs& in)
{
	InterlockedIncrement(&s_flagged);
	switch (action)
	{
	case TC_USE_CONTAINING: InterlockedIncrement(&s_containing); break;
	case TC_USE_NULL_TOWN:  InterlockedIncrement(&s_nullTown);   break;
	default:                InterlockedIncrement(&s_pass);       break;
	}
	if (action != TC_PASS && !in.townIsNull)
		InterlockedIncrement(&s_snapNpc);
}

#ifdef KEO_DEBUG
static void LogDecision(TownClaimAction action, const TownClaimInputs& in, void* was, void* town,
                        void* building, bool firstTime)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "TownClaim: ");
	FlbStr(&o, action == TC_USE_CONTAINING ? "containing"
	         : action == TC_USE_NULL_TOWN  ? "noTown" : "pass");
	FlbStr(&o, " was=");      FlbHex(&o, (unsigned __int64)(uintptr_t)was);
	FlbStr(&o, " town=");     FlbHex(&o, (unsigned __int64)(uintptr_t)town);
	FlbStr(&o, " snap=");
	FlbStr(&o, !in.hasSnapTarget ? "none" : in.snapTargetIsPlayerOwned ? "player" : "npc");
	FlbStr(&o, " firstTime="); FlbDec(&o, firstTime ? 1 : 0);
	FlbStr(&o, " building="); FlbHex(&o, (unsigned __int64)(uintptr_t)building);
	FlbStr(&o, " skips=");    FlbDec(&o, InterlockedCompareExchange(&s_recheckSkips, 0, 0));
	FlbStr(&o, " flagged=");  FlbDec(&o, InterlockedCompareExchange(&s_flagged, 0, 0));
	LogMsgDeferrable(FlbDone(&o));
}
#endif

// Main thread. Outside the builder's call (load, spawns, NPC buildings, every nested part
// creation) it is one compare and the original. Inside it, the town and the flag follow
// TownClaimDecide; the flag is restored after the call on every return path.
static void* __fastcall hook_createBuilding(void* factory, void* data, const float* position, void* town,
	void* owner, const float* rotation, void* cb, void* furnitureOf, void* isDoorOf,
	void* saveState, void* isIndoorsOf, bool invisible, bool completed, bool isFoliage,
	int floorNumber, bool isOutsideFurniture)
{
	if (t_placeDepth <= 0)
		return orig_createBuilding(factory, data, position, town, owner, rotation, cb, furnitureOf,
			isDoorOf, saveState, isIndoorsOf, invisible, completed, isFoliage, floorNumber,
			isOutsideFurniture);
	TownClaimInputs in;
	in.depthSet = true;
	in.ownerIsPlayer = FactionIsPlayer(owner);
	in.isFoliage = isFoliage;
	in.hasFurnitureOf = furnitureOf != NULL;
	in.hasDoorOf = isDoorOf != NULL;
	in.hasIndoorsOf = isIndoorsOf != NULL;
	in.hasSaveState = saveState != NULL;
	in.townIsNull = town == NULL;
	in.townIsPlayerTown = town != NULL && FactionIsPlayer(ObjectFaction(town));
	void* target = SnapTargetOf(cb);
	in.hasSnapTarget = target != NULL;
	in.snapTargetIsPlayerOwned = target != NULL && FactionIsPlayer(ObjectFaction(target));
	in.haveContainingPlayerTown = false;
	void* containing = NULL;
	if (TownClaimNeedsTown(in) && position)
	{
		containing = FindContainingPlayerTown(owner, position);
		in.haveContainingPlayerTown = containing != NULL;
	}
	const TownClaimAction action = TownClaimDecide(in);
	void* useTown = action == TC_USE_CONTAINING ? containing
	              : action == TC_USE_NULL_TOWN ? NullTown() : town;
	// noTown with no TownList: vanilla's own town, and the call goes on unflagged as vanilla's.
	const bool noList = action == TC_USE_NULL_TOWN && useTown == NULL;
	if (noList)
	{
		useTown = town;
		InterlockedIncrement(&s_nullList);
	}
	const bool flagged = !noList && TownClaimFlagged(in) && useTown != NULL;
	if (flagged)
		CountDecision(action, in);
	const int askedBefore = t_gateAsked;
	void* b;
	{
		SkipFlagScope skip(flagged ? 1 : 0);
		b = orig_createBuilding(factory, data, position, useTown, owner, rotation, cb, furnitureOf,
			isDoorOf, saveState, isIndoorsOf, invisible, completed, isFoliage, floorNumber,
			isOutsideFurniture);
	}
#ifdef KEO_DEBUG
	if (flagged)
		LogDecision(action, in, town, useTown, b, t_gateAsked != askedBefore);
#else
	(void)askedBefore;
#endif
	return b;
}

// Main thread, from the patched site in createBuilding: the placement's flag, cleared, so a
// later first-time check in the same call (or a nested one) re-checks as vanilla does.
extern "C" int __fastcall KEO_TownClaimGate(void)
{
	++t_gateAsked;
	int v = t_townClaimSkip;
	t_townClaimSkip = 0;
	if (v)
		InterlockedIncrement(&s_recheckSkips);
	return v;
}

namespace fixes {

void InstallTownClaim(int* installed, int*)
{
	if (!HookRowWanted(HOOK_BUILDER_PLACE)) return;
	// The snap test compares a callback's vftable with the one the builder stores; a binary
	// whose builder stores another refuses both rows.
	const char* row = "bytes";
	const char* why = NULL;
	const unsigned char* lea = (const unsigned char*)GameAddr(RVA_BUILDER_PLACE) + kTownClaimSnapLeaOffset;
	if (!TownClaimLeaReaches(lea, (unsigned __int64)(uintptr_t)lea,
	                         (unsigned __int64)(uintptr_t)GameAddr(RVA_SET_MOUNTED_CALLBACK_VTABLE)))
		why = "snap callback vftable";
	if (!why)
	{
		row = "builderPlace";
		why = HookInstall(HOOK_BUILDER_PLACE, hook_builderPlace, &orig_builderPlace, installed, true);
	}
	if (!why)
	{
		row = "createBuilding";
		why = HookInstall(HOOK_CREATE_BUILDING, hook_createBuilding, &orig_createBuilding,
		                  installed, true);
	}
	if (!why)
	{
		s_rowsInstalled = true;
		LogMsg("Town claim: hooks installed");
		return;
	}
	ErrorLog(std::string("Town claim: not installed (") + row + ": " + why
	         + "); a placement near an NPC town keeps vanilla's town");
}

bool TownClaimRowsInstalled()
{
	return s_rowsInstalled;
}

void TownClaimTick(double now)
{
	if (!g_fixesCfg.townClaimFixEnabled) return;
#ifdef KEO_DEBUG
	static double  s_nextBeat  = 0.0;
	static __int64 s_lastTotal = -1;   // the counters' sum at the last line
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + 60.0;

	const long flagged   = InterlockedCompareExchange(&s_flagged, 0, 0);
	const long pass      = InterlockedCompareExchange(&s_pass, 0, 0);
	const long contain   = InterlockedCompareExchange(&s_containing, 0, 0);
	const long nullTown  = InterlockedCompareExchange(&s_nullTown, 0, 0);
	const long nullList  = InterlockedCompareExchange(&s_nullList, 0, 0);
	const long snapNpc   = InterlockedCompareExchange(&s_snapNpc, 0, 0);
	const long candCap   = InterlockedCompareExchange(&s_candidateCap, 0, 0);
	const long skips     = InterlockedCompareExchange(&s_recheckSkips, 0, 0);
	// The counters only rise, so an unchanged sum means nothing moved since the last line.
	const __int64 total = (__int64)flagged + pass + contain + nullTown + nullList + snapNpc
	                    + candCap + skips;
	if (total == s_lastTotal || (s_lastTotal < 0 && total == 0))
		return;
	s_lastTotal = total;

	const int state = TownClaimPatchState();
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "TownClaim: heartbeat patch=");
	FlbStr(&o, state > 0 ? "armed" : state < 0 ? "refused" : "off");
	FlbStr(&o, " flagged=");      FlbDec(&o, flagged);
	FlbStr(&o, " pass=");         FlbDec(&o, pass);
	FlbStr(&o, " containing=");   FlbDec(&o, contain);
	FlbStr(&o, " nullTown=");     FlbDec(&o, nullTown);
	FlbStr(&o, " nullList=");     FlbDec(&o, nullList);
	FlbStr(&o, " snapNpc=");      FlbDec(&o, snapNpc);
	FlbStr(&o, " candidateCap="); FlbDec(&o, candCap);
	FlbStr(&o, " skips=");        FlbDec(&o, skips);
	LogMsg(FlbDone(&o));
#else
	(void)now;
#endif
}

} // namespace fixes
