#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/mesh_face_guard.h"
#include "fixes/streaming/mesh_face_guard_policy.h"
#include "base/fixed_log_buf.h"
#include "game/game.h"
#include "base/core.h"
#include "plugin/hook_manifest.h"
#include "base/config.h"
#include <windows.h>
#include "fixes/guard_report.h"
#include <string>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>                  // ErrorLog
#include "base/klib_include_end.h"

// judged + unjudged + fired == calls: every entry takes exactly one arm.
// unjudged is an object whose first pointer is not the instance vtable, which
// the detour hands straight to the original rather than measuring it against
// offsets that would mean nothing there. faceIdx + edgeRun + vertIdx + nullRec
// == fired.
static volatile LONG s_calls    = 0;
static volatile LONG s_judged   = 0;  // both tests passed; the original ran
static volatile LONG s_unjudged = 0;  // not an instance; nothing was tested
static volatile LONG s_fired    = 0;
static volatile LONG s_faceIdx  = 0;  //   because the face index was out of range
static volatile LONG s_edgeRun  = 0;  //   because the face's edge run was
static volatile LONG s_vertIdx  = 0;  //   because an edge's vertex index was
static volatile LONG s_nullRec  = 0;  //   because an accessor returned no record
static volatile LONG s_firstSec = -1;
static volatile LONG s_lastSec  = -1;
static volatile LONG s_lastFace = -1;

static volatile LONG s_fireLines = 0;
static const LONG kMaxFireLines  = 32;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

// The AABB seed pair the step starts its min and max accumulators from. It is
// a runtime-initialised constant -- zero in the image -- so it is read from
// the game's own data rather than reproduced, and the max half is its sign
// flip, exactly as the step builds it.
static const float* s_aabbSeed  = NULL;
static const void*  s_instVtbl  = NULL;

// The instance's own face accessor, called here so the record the guard judges
// is the record the original will fetch. Pure: it resolves an index through
// the face map and returns an address.
typedef __int64 (*faceFromIndex_t)(__int64 originalFaces, int numOriginalFaces,
                                   void* instancedFaces, void* ownedFaces,
                                   __int64 faceMap, int faceIndex);
static faceFromIndex_t fn_faceFromIndex = NULL;

// The instance's own edge accessor, same shape as the face accessor above,
// called here to pre-scan the vertex index each edge of a face's run names
// before the original walks them.
typedef __int64 (*edgeFromIndex_t)(__int64 originalEdges, int numOriginalEdges,
                                   void* instancedEdges, void* ownedEdges,
                                   __int64 edgeMap, int edgeIndex);
static edgeFromIndex_t fn_edgeFromIndex = NULL;

typedef void* (*faceAabb_t)(void* instance, int faceIndex, void* expand, void* out);
static faceAabb_t orig_faceAabb = NULL;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

static const GuardCounter kBeatRows[] =
{
	{ "calls",    GF_COUNT,    &s_calls,     0 },
	{ "judged",   GF_COUNT,    &s_judged,    0 },
	{ "unjudged", GF_COUNT,    &s_unjudged,  0 },
	{ "fired",    GF_COUNT,    &s_fired,     0 },
	{ "faceIdx",  GF_COUNT,    &s_faceIdx,   0 },
	{ "edgeRun",  GF_COUNT,    &s_edgeRun,   0 },
	{ "vertIdx",  GF_COUNT,    &s_vertIdx,   0 },
	{ "nullRec",  GF_COUNT,    &s_nullRec,   0 },
	{ "lines",    GF_COUNT_OF, &s_fireLines, kMaxFireLines },
	{ "firstSec", GF_COUNT,    &s_firstSec,  0 },
	{ "lastSec",  GF_COUNT,    &s_lastSec,   0 },
	{ "lastFace", GF_COUNT,    &s_lastFace,  0 },
};

static void EmitHeartbeat()
{
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "MeshFaceGuard running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

// Unconditional, on a timer: a guard that spoke only when it fired would make
// "never fired" and "never installed on a live site" the same silence. The
// clock is read on the first call and then once every 1024, because this site
// runs per face rather than per teardown.
static void MaybeHeartbeat(LONG calls)
{
	if (GuardBeatSample(calls) && GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

// Which section, which face and what the record claimed. A count alone cannot
// say whether the numbers are small overruns -- an index space read one step
// behind the instance -- or the wild values a freed backing store produces.
// haveEdges/haveVerts are passed by the caller rather than derived from arm:
// a nullRec fire can land either before the edge fields exist (the face
// accessor itself returned nothing) or after (one edge accessor did), and
// only the caller knows which.
static void EmitFireLine(MeshFaceArm arm, unsigned int section, int faceIndex,
                         __int64 faceSpace, bool haveEdges, int startEdge, int numEdges,
                         __int64 edgeSpace, bool haveVerts, int vertIdx, __int64 vertSpace)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "MeshFaceGuard FIRED: rva=0x00CFD724 ");
	switch (arm)
	{
	case MESH_FACE_INDEX_OUT_OF_RANGE:  FlbStr(&o, "faceIdx"); break;
	case MESH_FACE_EDGE_OUT_OF_RANGE:   FlbStr(&o, "edgeRun"); break;
	case MESH_FACE_VERTEX_OUT_OF_RANGE: FlbStr(&o, "vertIdx"); break;
	default:                            FlbStr(&o, "nullRec"); break;
	}
	FlbStr(&o, " section=");   FlbDec(&o, (__int64)(long)section);
	FlbStr(&o, " face=");      FlbDec(&o, faceIndex);
	FlbStr(&o, " faceSpace="); FlbDec(&o, faceSpace);
	FlbStr(&o, " startEdge="); if (haveEdges) FlbDec(&o, startEdge); else FlbChar(&o, '-');
	FlbStr(&o, " numEdges=");  if (haveEdges) FlbDec(&o, numEdges);  else FlbChar(&o, '-');
	FlbStr(&o, " edgeSpace="); if (haveEdges) FlbDec(&o, edgeSpace); else FlbChar(&o, '-');
	FlbStr(&o, " vert=");      if (haveVerts) FlbDec(&o, vertIdx);   else FlbChar(&o, '-');
	FlbStr(&o, " vertSpace="); if (haveVerts) FlbDec(&o, vertSpace); else FlbChar(&o, '-');
	FlbStr(&o, "; the face contributes no bounds. fired="); FlbDec(&o, Read(&s_fired));
	FlbStr(&o, " tid="); FlbDec(&o, (__int64)GetCurrentThreadId());
	LogMsgDeferrable(FlbDone(&o));
}

static void* hook_faceAabb(void* instance, int faceIndex, void* expand, void* out)
{
	LONG calls = InterlockedIncrement(&s_calls);

	const unsigned char* inst = (const unsigned char*)instance;
	if (!inst || *(const void* const*)inst != s_instVtbl)
	{
		InterlockedIncrement(&s_unjudged);
		MaybeHeartbeat(calls);
		return orig_faceAabb(instance, faceIndex, expand, out);
	}

	const int numFaces   = *(const int*)(inst + OFF_NMI_NUM_ORIGINAL_FACES);
	const int ownedFaces = *(const int*)(inst + OFF_NMI_OWNED_FACES + OFF_HKARRAY_SIZE);

	MeshFaceArm arm = ClassifyMeshFaceIndex(faceIndex, numFaces, ownedFaces);
	bool haveEdges = false;
	int startEdge = 0;
	int numEdges  = 0;
	__int64 edgeSpace = 0;
	int numEdgesTotal = 0;
	int ownedEdges = 0;
	bool haveVerts = false;
	int vertIdx = 0;
	__int64 vertSpace = 0;

	if (!MeshFaceArmSubstitutes(arm))
	{
		const __int64 face = fn_faceFromIndex(
			*(const __int64*)(inst + OFF_NMI_ORIGINAL_FACES), numFaces,
			(void*)(inst + OFF_NMI_INSTANCED_FACES), (void*)(inst + OFF_NMI_OWNED_FACES),
			(__int64)(inst + OFF_NMI_FACE_MAP), faceIndex);

		if (!face)
		{
			arm = MESH_FACE_NULL_RECORD;
		}
		else
		{
			startEdge = *(const int*)(face + OFF_FACE_START_EDGE);
			numEdges  = *(const short*)(face + OFF_FACE_NUM_EDGES);

			numEdgesTotal = *(const int*)(inst + OFF_NMI_NUM_ORIGINAL_EDGES);
			ownedEdges    = *(const int*)(inst + OFF_NMI_OWNED_EDGES + OFF_HKARRAY_SIZE);
			edgeSpace = MeshFaceIndexSpace(numEdgesTotal, ownedEdges);
			haveEdges = true;
			arm = ClassifyMeshFaceEdges(startEdge, numEdges, numEdgesTotal, ownedEdges);
		}
	}

	// m_originalEdges is itself one of the six arrays a freed backing store
	// clobbers, so a run that lands in range can still fetch a garbage edge
	// record. Pre-scan every vertex index the run names before the original
	// walks them; a face with no edges never reaches this loop either.
	if (!MeshFaceArmSubstitutes(arm) && numEdges > 0)
	{
		const int numVerts   = *(const int*)(inst + OFF_NMI_NUM_ORIGINAL_VERTICES);
		const int ownedVerts = *(const int*)(inst + OFF_NMI_OWNED_VERTICES + OFF_HKARRAY_SIZE);
		vertSpace = MeshFaceIndexSpace(numVerts, ownedVerts);

		for (int i = 0; i < numEdges; ++i)
		{
			const __int64 edge = fn_edgeFromIndex(
				*(const __int64*)(inst + OFF_NMI_ORIGINAL_EDGES), numEdgesTotal,
				(void*)(inst + OFF_NMI_INSTANCED_EDGES), (void*)(inst + OFF_NMI_OWNED_EDGES),
				(__int64)(inst + OFF_NMI_EDGE_MAP), startEdge + i);

			if (!edge)
			{
				arm = MESH_FACE_NULL_RECORD;
				break;
			}

			vertIdx = *(const int*)(edge + OFF_EDGE_VERTEX_INDEX);
			const MeshFaceArm vArm = ClassifyMeshFaceVertex(vertIdx, numVerts, ownedVerts);
			if (vArm != MESH_FACE_RUN_ORIGINAL)
			{
				haveVerts = true;
				arm = vArm;
				break;
			}
		}
	}

	if (!MeshFaceArmSubstitutes(arm))
	{
		InterlockedIncrement(&s_judged);
		// Emitted before the call that can fault, so a session that dies in it
		// still carries the totals.
		MaybeHeartbeat(calls);
		return orig_faceAabb(instance, faceIndex, expand, out);
	}

	const unsigned int section = *(const unsigned int*)(inst + OFF_NMI_SECTION_UID);
	InterlockedIncrement(&s_fired);
	switch (arm)
	{
	case MESH_FACE_INDEX_OUT_OF_RANGE:  InterlockedIncrement(&s_faceIdx); break;
	case MESH_FACE_EDGE_OUT_OF_RANGE:   InterlockedIncrement(&s_edgeRun); break;
	case MESH_FACE_VERTEX_OUT_OF_RANGE: InterlockedIncrement(&s_vertIdx); break;
	default:                            InterlockedIncrement(&s_nullRec); break;
	}
	InterlockedCompareExchange(&s_firstSec, (LONG)section, -1);
	InterlockedExchange(&s_lastSec, (LONG)section);
	InterlockedExchange(&s_lastFace, (LONG)faceIndex);

	// The bounds the step itself produces for a face whose edge loop never
	// runs, which is the one result its caller already handles.
	MeshFaceEmptyBounds(s_aabbSeed, (const float*)expand, (float*)out);

	if (GuardFireClaim(&s_fireLines, kMaxFireLines))
		EmitFireLine(arm, section, faceIndex, MeshFaceIndexSpace(numFaces, ownedFaces),
		             haveEdges, startEdge, numEdges, edgeSpace,
		             haveVerts, vertIdx, vertSpace);
	MaybeHeartbeat(calls);

	// The callers discard this; the original returns its expansion argument.
	return expand;
}

void InstallMeshFaceGuard(int* installed, int*)
{
	if (!HookRowWanted(HOOK_NAVMESH_FACE_AABB))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	s_aabbSeed       = (const float*)GameAddr(RVA_HKAI_AABB_SEED);
	s_instVtbl       = GameAddr(RVA_HKAI_NAVMESH_INSTANCE_VFTABLE);
	fn_faceFromIndex = (faceFromIndex_t)GameAddr(RVA_NAVMESH_FACE_FROM_INDEX);
	fn_edgeFromIndex = (edgeFromIndex_t)GameAddr(RVA_NAVMESH_EDGE_FROM_INDEX);

	const char* why = HookInstallRow(HOOK_NAVMESH_FACE_AABB, hook_faceAabb,
			(void**)&orig_faceAabb, installed, true);

	if (!why)
	{
		LogMsg("Mesh face guard: installed (a heartbeat line follows the first minute of cutting)");
		// A baseline line at zero calls, so a session that never cuts a section
		// reads as armed and quiet rather than as a session with no guard. The
		// timer is left unarmed, so the first call still beats.
		EmitHeartbeat();
	}
	else
	{
		orig_faceAabb = NULL;
		ErrorLog(std::string("Mesh face guard: not installed (") + why
		         + "); a face whose edge run leaves the instance still faults");
	}
}
