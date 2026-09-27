#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/stitch/unstitch_guard.h"
#include "fixes/stitch/unstitch_guard_policy.h"
#include "fixes/stitch/unstitch_layout.h"
#include "fixes/stitch/stitch_source.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/fixed_log_buf.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// ---------------------------------------------------------------------------
// Counters. Kept apart from the probe's: with both live the same record is
// seen twice, at two sites, and "the guard dropped a record" must never be
// read off a number the probe also moves.
// ---------------------------------------------------------------------------

static volatile LONG s_calls        = 0;  // detour entries
static volatile LONG s_scanned      = 0;  // the records were readable and were scanned
static volatile LONG s_unjudged     = 0;  // a header the scan could not trust; original called
static volatile LONG s_setsSeen     = 0;
static volatile LONG s_setsMatched  = 0;
static volatile LONG s_oppResolved  = 0;
static volatile LONG s_conns        = 0;  // records examined by the scan
static volatile LONG s_fired        = 0;  // calls that took the replacement walk
static volatile LONG s_skipOob      = 0;  // records the replacement dropped: index past the map
static volatile LONG s_skipMapVal   = 0;  // records it dropped: map value outside the node array
static volatile LONG s_replayed     = 0;  // records the replacement handed on unchanged
static volatile LONG s_implausible  = 0;  // a set whose length is not one to trust
static volatile LONG s_unreadable   = 0;

static volatile LONG s_fireLines    = 0;
static const LONG kMaxFireLines     = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// ---------------------------------------------------------------------------
// Fixed-buffer formatting: section-manager thread, so no CRT stream and no
// allocation.
// ---------------------------------------------------------------------------

typedef FixedLogBufN<512> GBuf;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// ---------------------------------------------------------------------------
// Guarded reads. Standalone and POD-only: MSVC 2010 rejects __try in a
// function that also holds objects needing unwinding.
// ---------------------------------------------------------------------------

static bool ReadI32(const void* at, int* out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const int*)at; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static bool ReadU8(const void* at, unsigned char* out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const unsigned char*)at; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static bool ReadPtr(const void* at, const unsigned char** out)
{
	bool ok = true;
	GuardEnter();
	__try { *out = *(const unsigned char* const*)at; }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

// The two halves of an instanced node: start edge and edge count.
static bool ReadNode(const void* at, int* startEdge, int* edgeCount)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*startEdge = *(const int*)at;
		*edgeCount = *(const int*)((const unsigned char*)at + 4);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

static bool WriteNodeCleared(void* at)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*(int*)at = -1;
		*(int*)((unsigned char*)at + 4) = 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	GuardLeave();
	return ok;
}

// ---------------------------------------------------------------------------
// Shared reads
// ---------------------------------------------------------------------------

struct SelfFacts
{
	int ownUid;
	int runtimeId;
	const unsigned char* setsData;
	int setsSize;
};

static bool ReadSelfFacts(const unsigned char* gi, SelfFacts* out)
{
	const unsigned char* graph = NULL;
	if (!ReadI32(gi + OFF_GI_UID, &out->ownUid)) return false;
	if (!ReadI32(gi + OFF_GI_RUNTIME_ID, &out->runtimeId)) return false;
	if (!ReadPtr(gi + OFF_GI_GRAPH, &graph) || !graph) return false;
	if (!ReadPtr(graph + OFF_GRAPH_SETS_DATA, &out->setsData)) return false;
	if (!ReadI32(graph + OFF_GRAPH_SETS_SIZE, &out->setsSize)) return false;
	return true;
}

struct SetFacts
{
	int thisUid;
	int oppUid;
	const unsigned char* connData;
	int connSize;
};

static bool ReadSetFacts(const unsigned char* entry, SetFacts* out)
{
	if (!ReadI32(entry + OFF_SET_THIS_UID, &out->thisUid)) return false;
	if (!ReadI32(entry + OFF_SET_OPP_UID, &out->oppUid)) return false;
	if (!ReadPtr(entry + OFF_SET_CONN_DATA, &out->connData)) return false;
	if (!ReadI32(entry + OFF_SET_CONN_SIZE, &out->connSize)) return false;
	return true;
}

// The collection's own uid lookup, re-expressed rather than called: every read
// is then inside this file's fault guard, and the scan enters no game code.
static bool ResolveOpposite(const unsigned char* coll, int uid, const unsigned char** out)
{
	int slotCount = 0;
	const unsigned char* slots = NULL;
	*out = NULL;
	if (!ReadI32(coll + OFF_COLL_SLOT_COUNT, &slotCount)) return false;
	if (!ReadPtr(coll + OFF_COLL_SLOTS, &slots)) return false;
	if (slotCount < 0 || slotCount > UNSTITCH_MAX_SETS || !slots)
		return false;
	for (int i = 0; i < slotCount; ++i)
	{
		const unsigned char* gi = NULL;
		int slotUid = 0;
		if (!ReadPtr(slots + COLL_SLOT_STRIDE * (size_t)i + OFF_COLL_SLOT_GRAPHINST, &gi))
			return false;
		if (!gi)
			continue;
		if (!ReadI32(gi + OFF_GI_UID, &slotUid))
			return false;
		if (slotUid == uid) { *out = gi; return true; }
	}
	return true;
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

// Answers one question: is there a recorded index the native walk is about to
// use that misses the opposite node map? Everything else it touches is a
// denominator, because "no offending record" is only meaningful against a
// count of records actually examined.
static bool ScanForOutOfBounds(const unsigned char* gi, const unsigned char* coll, bool* readable)
{
	*readable = true;

	SelfFacts self;
	self.ownUid = 0; self.runtimeId = 0; self.setsData = NULL; self.setsSize = 0;
	if (!ReadSelfFacts(gi, &self))
	{
		*readable = false;
		return false;
	}
	if (!UnstitchSetCountPlausible(self.setsSize) || (self.setsSize > 0 && !self.setsData))
	{
		*readable = false;
		return false;
	}

	bool found = false;
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
		if (action == UNSTITCH_SET_SKIP_IMPLAUSIBLE || (set.connSize > 0 && !set.connData))
		{
			InterlockedIncrement(&s_implausible);
			continue;
		}

		const unsigned char* opp = NULL;
		if (!ResolveOpposite(coll, set.oppUid, &opp))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}
		if (!opp)
			continue;  // no live opposite instance: the native walk skips too
		InterlockedIncrement(&s_oppResolved);

		int nodeMapSize = 0;
		if (!ReadI32(opp + OFF_GI_NODEMAP_SIZE, &nodeMapSize))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}

		for (int j = 0; j < set.connSize; ++j)
		{
			InterlockedIncrement(&s_conns);
			int nodeIndex = 0;
			if (!ReadI32(set.connData + CONN_STRIDE * (size_t)j + OFF_CONN_OPP_NODE, &nodeIndex))
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}
			if (UnstitchGuardConnAction(nodeIndex, nodeMapSize) == UNSTITCH_CONN_SKIP_OOB)
				found = true;
		}
	}
	return found;
}

// ---------------------------------------------------------------------------
// The replacement walk
// ---------------------------------------------------------------------------
// Reached only when the scan found a record that faults the native walk, so
// every path through here is one that would otherwise have died. It repeats
// the native walk step for step and drops the offending records; the two game
// functions it calls are the two the native walk calls, with the arguments the
// native walk would have passed.

typedef void (*releaseFreeBlocks_t)(void* graphInstance);
typedef void (*removeInstancedEdge_t)(void* graphInstance, int nodeIndex, int edgeIndex);

static releaseFreeBlocks_t   fn_releaseFreeBlocks   = NULL;
static removeInstancedEdge_t fn_removeInstancedEdge = NULL;

static void ClearInstancedNodes(const unsigned char* gi)
{
	int count = 0;
	if (!ReadI32(gi + OFF_GI_INSTNODES_SIZE, &count))
	{
		InterlockedIncrement(&s_unreadable);
		return;
	}
	for (int i = 0; i < count; ++i)
	{
		const unsigned char* data = NULL;
		if (!ReadPtr(gi + OFF_GI_INSTNODES_DATA, &data) || !data)
		{
			InterlockedIncrement(&s_unreadable);
			return;
		}
		if (!WriteNodeCleared((void*)(data + INSTNODE_STRIDE * (size_t)i)))
		{
			InterlockedIncrement(&s_unreadable);
			return;
		}
	}
}

// The edge the record names, if the opposite instance still carries it. The
// native scan starts at the node's first edge and gives up after its edge
// count, and the edge array is biased by the instance's original-edge count.
static bool FindInstancedEdge(const unsigned char* opp, int startEdge, int edgeCount,
                              int wantKey, int* edgeOut)
{
	for (int n = 0; n < edgeCount; ++n)
	{
		const unsigned char* edges = NULL;
		int bias = 0;
		if (!ReadPtr(opp + OFF_GI_OWNEDEDGES_DATA, &edges) || !edges)
			return false;
		if (!ReadI32(opp + OFF_GI_NUM_ORIG_EDGES, &bias))
			return false;

		int edge = startEdge + n;
		const unsigned char* e = edges + EDGE_STRIDE * (size_t)(edge - bias);
		unsigned char flags = 0;
		int key = 0;
		if (!ReadU8(e + OFF_EDGE_FLAGS, &flags) || !ReadI32(e + OFF_EDGE_KEY, &key))
			return false;
		if ((flags & EDGE_FLAG_LIVE) != 0 && key == wantKey)
		{
			*edgeOut = edge;
			return true;
		}
	}
	return false;
}

static void ReplacementUnstitch(const unsigned char* gi, const unsigned char* coll)
{
	fn_releaseFreeBlocks((void*)gi);
	ClearInstancedNodes(gi);

	SelfFacts self;
	self.ownUid = 0; self.runtimeId = 0; self.setsData = NULL; self.setsSize = 0;
	if (!ReadSelfFacts(gi, &self) || !UnstitchSetCountPlausible(self.setsSize)
		|| (self.setsSize > 0 && !self.setsData))
	{
		InterlockedIncrement(&s_unreadable);
		return;
	}

	for (int i = 0; i < self.setsSize; ++i)
	{
		SetFacts set;
		set.thisUid = 0; set.oppUid = 0; set.connData = NULL; set.connSize = 0;
		if (!ReadSetFacts(self.setsData + SET_STRIDE * (size_t)i, &set))
		{
			InterlockedIncrement(&s_unreadable);
			continue;
		}
		if (UnstitchClassifySet(set.thisUid, self.ownUid, set.connSize) != UNSTITCH_SET_WALK)
			continue;
		if (set.connSize > 0 && !set.connData)
			continue;

		const unsigned char* opp = NULL;
		if (!ResolveOpposite(coll, set.oppUid, &opp) || !opp)
			continue;

		// What this set lost, for the write-side join once the set is done.
		int drops = 0, minDrop = 0, maxDrop = 0, dropMapSize = 0;

		for (int j = 0; j < set.connSize; ++j)
		{
			// The opposite instance's arrays are re-read on every record
			// because the edge removal below reallocates them. The set's own
			// data and count are not: nothing this walk calls writes a
			// streaming set.
			const unsigned char* rec = set.connData + CONN_STRIDE * (size_t)j;
			int edgeKey = 0, nodeIndex = 0, nodeMapSize = 0;
			const unsigned char* nodeMap = NULL;
			if (!ReadI32(rec + OFF_CONN_EDGE_KEY, &edgeKey)
				|| !ReadI32(rec + OFF_CONN_OPP_NODE, &nodeIndex)
				|| !ReadI32(opp + OFF_GI_NODEMAP_SIZE, &nodeMapSize))
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}
			if (nodeMapSize <= 0)
				continue;  // the native walk short-circuits before the indexed read
			if (UnstitchGuardConnAction(nodeIndex, nodeMapSize) == UNSTITCH_CONN_SKIP_OOB)
			{
				InterlockedIncrement(&s_skipOob);
				if (drops == 0 || nodeIndex < minDrop) minDrop = nodeIndex;
				if (drops == 0 || nodeIndex > maxDrop) maxDrop = nodeIndex;
				if (drops == 0) dropMapSize = nodeMapSize;
				++drops;
				continue;
			}
			if (!ReadPtr(opp + OFF_GI_NODEMAP_DATA, &nodeMap) || !nodeMap)
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}

			int mapValue = 0;
			int instNodeCount = 0;
			if (!ReadI32(nodeMap + 4 * (size_t)nodeIndex, &mapValue)
				|| !ReadI32(opp + OFF_GI_INSTNODES_SIZE, &instNodeCount))
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}
			// The load that actually faulted. The scan does not trigger on
			// this one -- an index past the end of the node map is measured, a
			// map value outside the node array is not -- but once this walk
			// owns the records it does not make blind the read the native code
			// makes blind.
			UnstitchMapValueAction mv = UnstitchGuardMapValue(mapValue, instNodeCount);
			if (mv == UNSTITCH_MAP_NATIVE_SENTINEL)
				continue;  // nothing instanced for this node, as the native walk decides
			if (mv == UNSTITCH_MAP_SKIP_OUT_OF_RANGE)
			{
				InterlockedIncrement(&s_skipMapVal);
				continue;
			}

			const unsigned char* instNodes = NULL;
			int startEdge = 0, edgeCount = 0;
			if (!ReadPtr(opp + OFF_GI_INSTNODES_DATA, &instNodes) || !instNodes
				|| !ReadNode(instNodes + INSTNODE_STRIDE * (size_t)mapValue, &startEdge, &edgeCount))
			{
				InterlockedIncrement(&s_unreadable);
				continue;
			}
			InterlockedIncrement(&s_replayed);
			if (edgeCount <= 0)
				continue;

			int edge = 0;
			int wantKey = edgeKey | (self.runtimeId << EDGE_KEY_RUNTIME_SHIFT);
			if (FindInstancedEdge(opp, startEdge, edgeCount, wantKey, &edge))
				fn_removeInstancedEdge((void*)opp, nodeIndex, edge);
		}

		if (drops > 0)
			StitchSourceOnSetDrops(gi, self.ownUid, set.oppUid, set.connSize, opp,
			                       dropMapSize, minDrop, maxDrop, drops);
	}
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

static const GuardCounter kBeatRows[] =
{
	{ "calls",       GF_COUNT, &s_calls,       0 },
	{ "scanned",     GF_COUNT, &s_scanned,     0 },
	{ "unjudged",    GF_COUNT, &s_unjudged,    0 },
	{ "sets",        GF_COUNT, &s_setsSeen,    0 },
	{ "ours",        GF_COUNT, &s_setsMatched, 0 },
	{ "opp",         GF_COUNT, &s_oppResolved, 0 },
	{ "conns",       GF_COUNT, &s_conns,       0 },
	{ "fired",       GF_COUNT, &s_fired,       0 },
	{ "skipOob",     GF_COUNT, &s_skipOob,     0 },
	{ "skipMapVal",  GF_COUNT, &s_skipMapVal,  0 },
	{ "replayed",    GF_COUNT, &s_replayed,    0 },
	{ "implausible", GF_COUNT, &s_implausible, 0 },
	{ "unreadable",  GF_COUNT, &s_unreadable,  0 },
};

static void EmitHeartbeat()
{
	GBuf o;
	GuardHeartbeatBegin(&o, "UnstitchGuard running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer and on the first teardown. A guard that only spoke
// when it fired would make "never fired" and "never installed on a live site"
// the same silence.
static void MaybeHeartbeat()
{
	if (GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

// ---------------------------------------------------------------------------
// Detour
// ---------------------------------------------------------------------------

typedef void* (*unstitchCrossSection_t)(void* graphInstance, void* collection);
static unstitchCrossSection_t orig_unstitchCrossSection = NULL;

static bool s_guardInstalled = false;

bool UnstitchGuardInstalled()
{
	return s_guardInstalled;
}

static void* hook_unstitchCrossSection(void* graphInstance, void* collection)
{
	if (!graphInstance || !collection)
		return orig_unstitchCrossSection(graphInstance, collection);

	InterlockedIncrement(&s_calls);

	bool readable = true;
	bool found = ScanForOutOfBounds((const unsigned char*)graphInstance,
	                                (const unsigned char*)collection, &readable);
	if (readable)
		InterlockedIncrement(&s_scanned);
	else
		InterlockedIncrement(&s_unjudged);

	// Emitted before the call that can fault, so a session that dies anyway
	// still carries the totals.
	MaybeHeartbeat();

	if (!found)
		return orig_unstitchCrossSection(graphInstance, collection);

	InterlockedIncrement(&s_fired);
	if (InterlockedIncrement(&s_fireLines) <= kMaxFireLines)
	{
		GBuf o; o.n = 0;
		FlbStr(&o, "UnstitchGuard FIRED: a recorded node index misses the opposite node map; "
		          "walking without it. call="); FlbDec(&o, Read(&s_calls));
		FlbStr(&o, " fired="); FlbDec(&o, Read(&s_fired));
		FlbStr(&o, " tid=");   FlbDec(&o, (__int64)GetCurrentThreadId());
		LogMsgDeferrable(FlbDone(&o));
	}

	StitchSourceBeginFired(graphInstance, Read(&s_calls));
	ReplacementUnstitch((const unsigned char*)graphInstance,
	                    (const unsigned char*)collection);

	// Both callers discard this; the native walk returns the streaming-set
	// array it walked, so return the same thing rather than something new.
	const unsigned char* graph = NULL;
	if (ReadPtr((const unsigned char*)graphInstance + OFF_GI_GRAPH, &graph) && graph)
		return (void*)(graph + OFF_GRAPH_SETS_DATA);
	return NULL;
}

void InstallUnstitchGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_UNSTITCH_CROSS_SECTION))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	fn_releaseFreeBlocks   = (releaseFreeBlocks_t)GameAddr(RVA_GRAPHINST_RELEASE_FREE_BLOCKS);
	fn_removeInstancedEdge = (removeInstancedEdge_t)GameAddr(RVA_GRAPHINST_REMOVE_INSTANCED_EDGE);

	const char* why = HookInstallRow(HOOK_UNSTITCH_CROSS_SECTION, hook_unstitchCrossSection,
			(void**)&orig_unstitchCrossSection, installed, true);

	if (!why)
	{
		s_guardInstalled = true;
		LogMsg("Un-stitch bounds guard: installed (a heartbeat line follows the first teardown)");
	}
	else
	{
		orig_unstitchCrossSection = NULL;
		ErrorLog(std::string("Un-stitch bounds guard: not installed (") + why
		         + "); a teardown that meets a stale connection record still faults");
	}
}
