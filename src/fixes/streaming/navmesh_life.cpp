#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/navmesh_life.h"
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

// capZero + capEq + capGt + noMesh + unread + vtblBad + addOther == adds: every
// registration takes exactly one arm, and capZero is kept out of capEq because
// an empty mesh satisfies capacity == count without having been rebuilt, which
// is the one arm that must not collect false positives. rets counts retires,
// of which retOther
// is the same "not an instance" arm; the two totals are kept apart so an
// add/retire rate can be compared between configurations -- that comparison is
// the reason none of this is gated on any preload key.
static volatile LONG s_adds     = 0;
static volatile LONG s_rets     = 0;
static volatile LONG s_capZero  = 0;  // no faces at all: neither reading applies
static volatile LONG s_capEq    = 0;  // capacity == count: a rebuilt mesh
static volatile LONG s_capGt    = 0;  // capacity  > count: a mesh the engine loaded
static volatile LONG s_noMesh   = 0;  // the instance holds no mesh pointer
static volatile LONG s_unread   = 0;  // the mesh could not be read at all
static volatile LONG s_vtblBad  = 0;  // it could, and it is not a mesh any more
static volatile LONG s_dies     = 0;  // retires at which the mesh loses its last holder
static volatile LONG s_addOther = 0;  // registered object is not a navmesh instance
static volatile LONG s_retOther = 0;  //   the same at retire
static volatile LONG s_resident = 0;  // sections in the collection at the last row

// Capped separately, because a session registers and retires hundreds of
// sections and every ADD precedes its own RET: one shared cap would spend
// itself on the ADD rows and truncate exactly the retire rows the refcounts
// and the dies= verdict live in.
static volatile LONG s_addRows = 0;
static volatile LONG s_retRows = 0;
static const LONG kMaxAddRows  = 512;
static const LONG kMaxRetRows  = 512;

static LONGLONG s_qpf = 0;
static volatile LONGLONG s_nextBeat = 0;
static const int kBeatSeconds = 60;

static const void* s_instVtbl = NULL;
static const void* s_meshVtbl = NULL;

// The collection's section count: how many sections are resident right now.
// An add/retire rate rises both when more cells are resident and when the same
// cells churn faster, and only this number separates them.
static const size_t OFF_COLLECTION_COUNT = 40;

typedef __int64 (*removeInstance_t)(__int64 collection, __int64 instance, __int64 graphInstance);
static removeInstance_t orig_removeInstance = NULL;

static LONG Read(volatile LONG* p) { return InterlockedCompareExchange(p, 0, 0); }

// Saturating: once the cap is reached the counter stops moving, so the number
// in the heartbeat stays the number of rows actually written.
static bool ClaimRow(volatile LONG* counter, LONG cap)
{
	for (;;)
	{
		LONG cur = Read(counter);
		if (cur >= cap)
			return false;
		if (InterlockedCompareExchange(counter, cur + 1, cur) == cur)
			return true;
	}
}

// What one read of a mesh produced. Kept raw: the derived answer is one bit
// and a wrong derivation must not be able to destroy the evidence behind it.
struct MeshRead
{
	const void* mesh;
	int         refCount;   // -1 when not read
	int         faceCount;  // -1 when not read
	int         faceCap;    // -1 when not read
	bool        readable;
	bool        isMesh;
};

static void ReadMesh(const unsigned char* mesh, MeshRead* out)
{
	out->mesh      = mesh;
	out->refCount  = -1;
	out->faceCount = -1;
	out->faceCap   = -1;
	out->readable  = false;
	out->isMesh    = false;
	if (!mesh)
		return;
	__try
	{
		const void* vtbl = *(const void* const*)mesh;
		out->readable = true;
		out->isMesh   = (vtbl == s_meshVtbl);
		out->refCount = *(const unsigned short*)(mesh + OFF_REFOBJ_COUNT);
		out->faceCount = *(const int*)(mesh + OFF_NMI_ORIGINAL_FACES + OFF_HKARRAY_SIZE);
		out->faceCap   = (int)(*(const unsigned int*)(mesh + OFF_NMI_ORIGINAL_FACES + OFF_HKARRAY_CAP)
		                       & HKARRAY_CAP_MASK);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out->readable = false;
		out->isMesh   = false;
	}
}

// Counts the arm an add takes. Retires take the same arms without recounting
// them, so the identity above stays over adds alone.
static void TallyArm(const MeshRead* m)
{
	if (!m->mesh)
		InterlockedIncrement(&s_noMesh);
	else if (!m->readable)
		InterlockedIncrement(&s_unread);
	else if (!m->isMesh)
		InterlockedIncrement(&s_vtblBad);
	else if (m->faceCount <= 0 && m->faceCap <= 0)
		InterlockedIncrement(&s_capZero);
	else if (m->faceCap == m->faceCount)
		InterlockedIncrement(&s_capEq);
	else
		InterlockedIncrement(&s_capGt);
}

static const GuardCounter kBeatRows[] =
{
	{ "adds",     GF_COUNT,    &s_adds,     0 },
	{ "rets",     GF_COUNT,    &s_rets,     0 },
	{ "capZero",  GF_COUNT,    &s_capZero,  0 },
	{ "capEq",    GF_COUNT,    &s_capEq,    0 },
	{ "capGt",    GF_COUNT,    &s_capGt,    0 },
	{ "noMesh",   GF_COUNT,    &s_noMesh,   0 },
	{ "unread",   GF_COUNT,    &s_unread,   0 },
	{ "vtblBad",  GF_COUNT,    &s_vtblBad,  0 },
	{ "addOther", GF_COUNT,    &s_addOther, 0 },
	{ "retOther", GF_COUNT,    &s_retOther, 0 },
	{ "dies",     GF_COUNT,    &s_dies,     0 },
	{ "resident", GF_COUNT,    &s_resident, 0 },
	{ "addRows",  GF_COUNT_OF, &s_addRows,  kMaxAddRows },
	{ "retRows",  GF_COUNT_OF, &s_retRows,  kMaxRetRows },
};

static void EmitHeartbeat()
{
	FixedLogBuf o;
	GuardHeartbeatBegin(&o, "NavMeshLife running:");
	GuardFields(&o, kBeatRows, (int)ARRAYSIZE(kBeatRows), true);
	LogMsgDeferrable(FlbDone(&o));
}

static void MaybeHeartbeat()
{
	if (GuardBeatDue(&s_nextBeat, s_qpf, kBeatSeconds)) EmitHeartbeat();
}

static void EmitRow(const char* what, const unsigned char* inst, int instRc,
                    const MeshRead* m, int resident, int dies)
{
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "NavMeshLife "); FlbStr(&o, what);
	FlbStr(&o, " sec=");  FlbDec(&o, (__int64)*(const unsigned int*)(inst + OFF_NMI_SECTION_UID));
	FlbStr(&o, " slot="); FlbDec(&o, *(const int*)(inst + OFF_NMI_RUNTIME_ID));
	FlbStr(&o, " inst="); FlbHex(&o, (unsigned __int64)inst);
	FlbStr(&o, " rcInst="); FlbDec(&o, instRc);
	FlbStr(&o, " mesh=");   FlbHex(&o, (unsigned __int64)m->mesh);
	FlbStr(&o, " rcMesh="); FlbDec(&o, m->refCount);
	FlbStr(&o, " faces=");  FlbDec(&o, m->faceCount);
	FlbStr(&o, "/");        FlbDec(&o, m->faceCap);
	FlbStr(&o, " readable="); FlbDec(&o, m->readable ? 1 : 0);
	FlbStr(&o, " isMesh=");   FlbDec(&o, m->isMesh ? 1 : 0);
	FlbStr(&o, " resident="); FlbDec(&o, resident);
	if (dies >= 0)
	{
		FlbStr(&o, " dies="); FlbDec(&o, dies);
	}
	LogMsgDeferrable(FlbDone(&o));
}

// Shared by both ends. Returns the instance's own reference count, or -1 when
// the object is not an instance, in which case no row is written.
static int Sample(const unsigned char* inst, const void* collection,
                  const char* what, int dies)
{
	if (!inst || *(const void* const*)inst != s_instVtbl)
	{
		InterlockedIncrement(dies < 0 ? &s_addOther : &s_retOther);
		return -1;
	}

	MeshRead m;
	ReadMesh(*(const unsigned char* const*)(inst + OFF_NMI_ORIGINAL_MESH), &m);

	const int instRc = *(const unsigned short*)(inst + OFF_REFOBJ_COUNT);
	int resident = -1;
	if (collection)
		resident = *(const int*)((const unsigned char*)collection + OFF_COLLECTION_COUNT);
	InterlockedExchange(&s_resident, (LONG)resident);

	int diesNow = dies;
	if (dies >= 0)
	{
		// The collection is about to drop its reference to the instance, and
		// the instance holds the only one the mesh gets from here. Predicted
		// rather than read back: reading the mesh after it can have been freed
		// is the very access this instrument exists to find.
		diesNow = (instRc <= 1 && m.readable && m.isMesh && m.refCount <= 1) ? 1 : 0;
		if (diesNow)
			InterlockedIncrement(&s_dies);
	}

	const bool isAdd = (dies < 0);
	if (ClaimRow(isAdd ? &s_addRows : &s_retRows, isAdd ? kMaxAddRows : kMaxRetRows))
		EmitRow(what, inst, instRc, &m, resident, diesNow);
	if (dies < 0)
		TallyArm(&m);
	return instRc;
}

void NavMeshLifeOnAdd(void* collection, __int64 instance)
{
	if (!fixes::g_fixesCfg.navMeshLifeEnabled)
		return;
	InterlockedIncrement(&s_adds);
	Sample((const unsigned char*)instance, collection, "ADD", -1);
	MaybeHeartbeat();
}

static __int64 hook_removeInstance(__int64 collection, __int64 instance, __int64 graphInstance)
{
	InterlockedIncrement(&s_rets);
	Sample((const unsigned char*)instance, (const void*)collection, "RET", 0);
	// Emitted before the original, so a session that dies in the removal still
	// carries the totals.
	MaybeHeartbeat();
	return orig_removeInstance(collection, instance, graphInstance);
}

void InstallNavMeshLife(int* installed, int*)
{
	if (!HookRowWanted(HOOK_REMOVE_INSTANCE))
		return;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	s_nextBeat = 0;

	s_instVtbl = GameAddr(RVA_HKAI_NAVMESH_INSTANCE_VFTABLE);
	s_meshVtbl = GameAddr(RVA_HKAI_NAVMESH_VFTABLE);

	const char* why = HookInstallRow(HOOK_REMOVE_INSTANCE, hook_removeInstance,
			(void**)&orig_removeInstance, installed, true);

	if (!why)
	{
		LogMsg("NavMesh lifecycle: installed (ADD and RET rows, then a heartbeat every minute)");
		// A baseline line at zero, so a session with no sections at all reads
		// as armed and quiet rather than as a session that never looked. The
		// timer is left unarmed, so the first registration still beats.
		EmitHeartbeat();
	}
	else
	{
		orig_removeInstance = NULL;
		ErrorLog(std::string("NavMesh lifecycle: retire half not installed (") + why
		         + "); ADD rows still appear, RET rows do not");
	}
}
