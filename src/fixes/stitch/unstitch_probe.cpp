#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/stitch/unstitch_probe.h"

#ifdef KEO_DEBUG

#include "fixes/stitch/unstitch_probe_policy.h"
#include "fixes/stitch/unstitch_layout.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include "base/config.h"
#include "navmesh/cache/nm_cache_core.h"   // workerBusyCount
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include "base/klib_include_end.h"

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// The streaming-set and graph-instance offsets the walk uses live in
// unstitch_layout.h, shared with the bounds guard so the two cannot drift.
// What is local here is the call the probe detours rather than the walk itself.

static const size_t OFF_NAVINST_GRAPHINST   = 0x18;  // NavInstance::graphInstance
static const size_t OFF_NAVINST_INSTANCE    = 0x28;  // NavInstance::instance
static const size_t OFF_MESHINST_RUNTIMEID  = 420;   // < 0 means never streamed in

static const size_t OFF_SM_WORLD            = 0x88;
static const size_t OFF_SM_CHANGEMUTEX      = 0x200;
static const size_t OFF_SM_ADDLIST_COUNT    = 0x278;
static const size_t OFF_SM_ADDLIST_STUFF    = 0x280;

static const size_t OFF_WORLD_COLLECTION    = 32;    // hkaiStreamingCollection*

// boost::shared_mutex state word: this bit is the exclusive hold.
static const unsigned long MUTEX_EXCLUSIVE  = 0x400000;

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

static volatile LONG s_calls        = 0;  // detour entries
static volatile LONG s_skipped      = 0;  // the original will not reach the un-stitch
static volatile LONG s_walked       = 0;  // it will, and the walk ran
static volatile LONG s_setsSeen     = 0;
static volatile LONG s_setsMatched  = 0;
static volatile LONG s_oppResolved  = 0;
static volatile LONG s_conns        = 0;
static volatile LONG s_rowsPastEnd  = 0;
static volatile LONG s_rowsNegative = 0;
static volatile LONG s_implausible  = 0;
static volatile LONG s_unreadable   = 0;
static volatile LONG s_lockHeldAfter = 0; // changeMutex still exclusive after the original

static volatile LONG s_rowLines     = 0;
static volatile LONG s_lockLines    = 0;
static const LONG kMaxRowLines      = 64;
static const LONG kMaxLockLines     = 8;

static LONGLONG s_qpf     = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 30;

// ---------------------------------------------------------------------------
// Fixed-buffer formatting: this runs on the section-manager thread, so no CRT
// stream and no allocation.
// ---------------------------------------------------------------------------

typedef FixedLogBufN<512> PBuf;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// ---------------------------------------------------------------------------
// Guarded reads. Standalone and POD-only: MSVC 2010 rejects __try in a
// function that also holds objects needing unwinding.
// ---------------------------------------------------------------------------

struct InstanceFacts
{
	int ownUid;
	const unsigned char* setsData;
	int setsSize;
};

struct SetFacts
{
	int thisUid;
	int oppUid;
	const unsigned char* connData;
	int connSize;
};

static bool ReadInstanceFacts(const void* graphInstance, InstanceFacts* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		const unsigned char* gi = (const unsigned char*)graphInstance;
		out->ownUid = *(const int*)(gi + OFF_GI_UID);
		const unsigned char* graph = *(const unsigned char* const*)(gi + OFF_GI_GRAPH);
		if (!graph)
		{
			ok = false;
		}
		else
		{
			out->setsData = *(const unsigned char* const*)(graph + OFF_GRAPH_SETS_DATA);
			out->setsSize = *(const int*)(graph + OFF_GRAPH_SETS_SIZE);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool ReadSetFacts(const unsigned char* entry, SetFacts* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		out->thisUid  = *(const int*)(entry + OFF_SET_THIS_UID);
		out->oppUid   = *(const int*)(entry + OFF_SET_OPP_UID);
		out->connData = *(const unsigned char* const*)(entry + OFF_SET_CONN_DATA);
		out->connSize = *(const int*)(entry + OFF_SET_CONN_SIZE);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// The collection's own uid lookup, re-expressed rather than called: every read
// is then inside this probe's fault guard, and nothing enters game code.
static bool ResolveOpposite(const void* collection, int uid, const unsigned char** out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		const unsigned char* coll = (const unsigned char*)collection;
		int slotCount = *(const int*)(coll + OFF_COLL_SLOT_COUNT);
		const unsigned char* slots = *(const unsigned char* const*)(coll + OFF_COLL_SLOTS);
		*out = NULL;
		if (slotCount < 0 || slotCount > UNSTITCH_MAX_SETS || !slots)
		{
			ok = false;
		}
		else
		{
			for (int i = 0; i < slotCount; ++i)
			{
				const unsigned char* gi =
					*(const unsigned char* const*)(slots + COLL_SLOT_STRIDE * i + OFF_COLL_SLOT_GRAPHINST);
				if (gi && *(const int*)(gi + OFF_GI_UID) == uid)
				{
					*out = gi;
					break;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool ReadNodeMapSize(const unsigned char* graphInstance, int* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		// The native guard reads the count and, when it is non-zero, the data
		// pointer. A null pointer with a non-zero count is a torn header, not
		// a record this probe can judge.
		*out = *(const int*)(graphInstance + OFF_GI_NODEMAP_SIZE);
		if (*out > 0 && !*(const unsigned char* const*)(graphInstance + OFF_GI_NODEMAP_DATA))
			ok = false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool ReadConnNodeIndex(const unsigned char* connData, int j, int* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const int*)(connData + CONN_STRIDE * (size_t)j + OFF_CONN_OPP_NODE);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// The original reaches the un-stitch only when the instance is not queued in
// addList and its runtime id is non-negative. Replicating both keeps the probe
// silent on the calls that never walk anything.
static bool ReadCallFacts(const void* sectionMgr, const void* navInstance,
                          bool* willUnstitch, const void** graphInstance,
                          const void** collection, unsigned long* mutexWord)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		const unsigned char* sm = (const unsigned char*)sectionMgr;
		const unsigned char* ni = (const unsigned char*)navInstance;

		*mutexWord = *(const volatile unsigned long*)(sm + OFF_SM_CHANGEMUTEX);
		*graphInstance = *(void* const*)(ni + OFF_NAVINST_GRAPHINST);

		const unsigned char* mesh = *(const unsigned char* const*)(ni + OFF_NAVINST_INSTANCE);
		*willUnstitch = false;
		if (mesh && *(const int*)(mesh + OFF_MESHINST_RUNTIMEID) >= 0)
		{
			bool queued = false;
			unsigned int count = *(const unsigned int*)(sm + OFF_SM_ADDLIST_COUNT);
			void* const* stuff = *(void* const* const*)(sm + OFF_SM_ADDLIST_STUFF);
			if (count > (unsigned int)UNSTITCH_MAX_SETS)
			{
				ok = false;  // not a length worth trusting; answer nothing
			}
			else
			{
				if (count && stuff)
				{
					for (unsigned int i = 0; i < count; ++i)
						if (stuff[i] == navInstance) { queued = true; break; }
				}
				*willUnstitch = !queued;
			}
		}

		const unsigned char* world = *(const unsigned char* const*)(sm + OFF_SM_WORLD);
		*collection = world ? *(void* const*)(world + OFF_WORLD_COLLECTION) : NULL;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

static bool ReadMutexWord(const void* sectionMgr, unsigned long* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const volatile unsigned long*)((const unsigned char*)sectionMgr + OFF_SM_CHANGEMUTEX);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

static const GuardCounter kBeatRows[] =
{
	{ "calls",         GF_COUNT, &s_calls,         0 },
	{ "skipped",       GF_COUNT, &s_skipped,       0 },
	{ "walked",        GF_COUNT, &s_walked,        0 },
	{ "sets",          GF_COUNT, &s_setsSeen,      0 },
	{ "ours",          GF_COUNT, &s_setsMatched,   0 },
	{ "opp",           GF_COUNT, &s_oppResolved,   0 },
	{ "conns",         GF_COUNT, &s_conns,         0 },
	{ "oob",           GF_COUNT, &s_rowsPastEnd,   0 },
	{ "negIdx",        GF_COUNT, &s_rowsNegative,  0 },
	{ "implausible",   GF_COUNT, &s_implausible,   0 },
	{ "unreadable",    GF_COUNT, &s_unreadable,    0 },
	{ "lockHeldAfter", GF_COUNT, &s_lockHeldAfter, 0 },
};

static void EmitHeartbeat()
{
	PBuf o;
	GuardHeartbeatBegin(&o, "UnstitchProbe running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer and on the first call: "no rows" only means
// anything against a denominator that says how much was actually examined.
static void MaybeHeartbeat()
{
	if (GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

static void EmitRow(int setIndex, int connIndex, const SetFacts& set,
                    int nodeIndex, int nodeMapSize, UnstitchNodeVerdict verdict,
                    unsigned long mutexWord)
{
	if (InterlockedIncrement(&s_rowLines) > kMaxRowLines)
		return;

	PBuf o; o.n = 0;
	FlbStr(&o, "UnstitchProbe ROW: ");
	FlbStr(&o, verdict == UNSTITCH_NODE_NEGATIVE ? "negIdx" : "oob");
	FlbStr(&o, " set=");       FlbDec(&o, setIndex);
	FlbStr(&o, " conn=");      FlbDec(&o, connIndex);
	FlbStr(&o, " thisUid=");   FlbDec(&o, set.thisUid);
	FlbStr(&o, " oppUid=");    FlbDec(&o, set.oppUid);
	FlbStr(&o, " nodeIndex="); FlbDec(&o, nodeIndex);
	FlbStr(&o, " nodeMapSize="); FlbDec(&o, nodeMapSize);
	FlbStr(&o, " busy=");
	FlbDec(&o, (LONG)InterlockedCompareExchange(&navmesh::g_nmCache.workerBusyCount, 0, 0));
	FlbStr(&o, " changeMutex=0x"); FlbHexDigits(&o, mutexWord, 8);
	FlbStr(&o, " tid=");       FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

// Walks the dying instance's streaming sets exactly as the native un-stitch
// will, and reports every recorded index that would land outside the opposite
// node map the read is about to use. Nothing here writes, locks or allocates.
static void WalkBeforeUnstitch(const void* graphInstance, const void* collection,
                               unsigned long mutexWord)
{
	InstanceFacts self;
	self.ownUid = 0; self.setsData = NULL; self.setsSize = 0;
	if (!ReadInstanceFacts(graphInstance, &self))
	{
		InterlockedIncrement(&s_unreadable);
		return;
	}
	if (!UnstitchSetCountPlausible(self.setsSize) || (self.setsSize > 0 && !self.setsData))
	{
		InterlockedIncrement(&s_implausible);
		return;
	}

	InterlockedIncrement(&s_walked);

	for (int i = 0; i < self.setsSize; ++i)
	{
		InterlockedIncrement(&s_setsSeen);

		SetFacts set;
		set.thisUid = 0; set.oppUid = 0; set.connData = NULL; set.connSize = 0;
		if (!ReadSetFacts(self.setsData + SET_STRIDE * (size_t)i, &set))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}

		UnstitchSetAction action = UnstitchClassifySet(set.thisUid, self.ownUid, set.connSize);
		if (action == UNSTITCH_SET_SKIP_NOT_OURS)
			continue;
		InterlockedIncrement(&s_setsMatched);
		if (action == UNSTITCH_SET_SKIP_IMPLAUSIBLE)
		{
			InterlockedIncrement(&s_implausible);
			continue;
		}
		if (set.connSize > 0 && !set.connData)
		{
			InterlockedIncrement(&s_implausible);
			continue;
		}

		const unsigned char* opp = NULL;
		if (!ResolveOpposite(collection, set.oppUid, &opp))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}
		if (!opp)
			continue;  // no live opposite instance: the native walk skips too
		InterlockedIncrement(&s_oppResolved);

		int nodeMapSize = 0;
		if (!ReadNodeMapSize(opp, &nodeMapSize))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}

		for (int j = 0; j < set.connSize; ++j)
		{
			InterlockedIncrement(&s_conns);

			int nodeIndex = 0;
			if (!ReadConnNodeIndex(set.connData, j, &nodeIndex))
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}

			UnstitchNodeVerdict v = UnstitchClassifyNodeIndex(nodeIndex, nodeMapSize);
			if (!UnstitchVerdictIsRow(v))
				continue;
			if (v == UNSTITCH_NODE_NEGATIVE)
				InterlockedIncrement(&s_rowsNegative);
			else
				InterlockedIncrement(&s_rowsPastEnd);
			EmitRow(i, j, set, nodeIndex, nodeMapSize, v, mutexWord);
		}
	}
}

// ---------------------------------------------------------------------------
// Detour
// ---------------------------------------------------------------------------

typedef unsigned __int64 (*deleteInstance_t)(void* sectionMgr, void* navInstance);
static deleteInstance_t orig_deleteInstance_probe = NULL;

static unsigned __int64 hook_deleteInstance_probe(void* sectionMgr, void* navInstance)
{
	// No flag test here: the detour exists only because the key was on when
	// the hooks installed, and nothing changes it afterwards.
	if (!sectionMgr || !navInstance)
		return orig_deleteInstance_probe(sectionMgr, navInstance);

	InterlockedIncrement(&s_calls);

	bool willUnstitch = false;
	const void* graphInstance = NULL;
	const void* collection = NULL;
	unsigned long mutexWord = 0;
	if (!ReadCallFacts(sectionMgr, navInstance, &willUnstitch, &graphInstance,
			&collection, &mutexWord))
	{
		InterlockedIncrement(&s_unreadable);
	}
	else if (!willUnstitch || !graphInstance || !collection)
	{
		InterlockedIncrement(&s_skipped);
	}
	else
	{
		// Before the original, so a fault inside the un-stitch cannot take the
		// evidence with it.
		WalkBeforeUnstitch(graphInstance, collection, mutexWord);
	}

	// After the walk, so the first line already carries this call's own totals,
	// and still before the call that can fault.
	MaybeHeartbeat();

	unsigned __int64 r = orig_deleteInstance_probe(sectionMgr, navInstance);

	// The original takes changeMutex with a system_time_max deadline, ignores
	// the result and unlocks unconditionally. The result itself cannot vary at
	// that deadline and is not visible from here; what an unlock without a hold
	// would leave behind is, and it is sticky -- the exclusive bit stays set
	// with no owner. A single hit is another thread mid-acquisition; the same
	// bit on call after call is the real thing.
	unsigned long after = 0;
	if (ReadMutexWord(sectionMgr, &after) && (after & MUTEX_EXCLUSIVE) != 0)
	{
		InterlockedIncrement(&s_lockHeldAfter);
		if (InterlockedIncrement(&s_lockLines) <= kMaxLockLines)
		{
			PBuf o; o.n = 0;
			FlbStr(&o, "UnstitchProbe LOCK: changeMutex still exclusive after deleteInstance, word=0x");
			FlbHexDigits(&o, after, 8);
			FlbStr(&o, " seen="); FlbDec(&o, Read(&s_lockHeldAfter));
			FlbStr(&o, " of calls="); FlbDec(&o, Read(&s_calls));
			FlbStr(&o, " tid="); FlbDec(&o, (__int64)GetCurrentThreadId());
			LogMsgDeferrable(FlbDone(&o));
		}
	}

	return r;
}

void InstallUnstitchProbe(int* installed, int*)
{
	if (!HookRowWanted(HOOK_NAVMESH_DELETE_INSTANCE))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	const char* why = HookInstall(HOOK_NAVMESH_DELETE_INSTANCE, hook_deleteInstance_probe,
			&orig_deleteInstance_probe, installed, true);

	if (!why)
	{
		LogMsg("Unstitch probe: installed (read-only; a heartbeat line follows the first teardown)");
	}
	else
	{
		orig_deleteInstance_probe = NULL;
		LogError(std::string("Unstitch probe: not installed (") + why + ")");
	}
}

#else  // KEO_DEBUG

// Nothing of this probe exists outside a DEV build; hook_manifest.cpp does not name it.
typedef int UnstitchProbeNotInThisBuild;

#endif // KEO_DEBUG
