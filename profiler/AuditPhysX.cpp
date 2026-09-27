// PhysX scene-query probe: samples the physics phase, run and core-lock owner per query.

#include "game/klib_members.h"
#include "KenshiFrameAudit_internal.h"
#include <string.h>

namespace audit
{

const char* PHYS_PHASE_NAMES[PP_COUNT] =
{
	"idle", "bodyOther", "lock", "preOther", "make", "group", "impulse",
	"hullDestroy", "actorDestroy", "terrain", "hullApply", "simulate",
	"flush", "fetch", "controller", "post"
};

const char* PHYS_QUERY_NAMES[PQ_KIND_COUNT] = { "mouseAll", "indoors" };

const char* PHYS_OWNER_NAMES[POW_COUNT] = { "unknown", "none", "main", "physBack", "ai", "other" };

uintptr_t CurrentNpScene()
{
	if (!g_cursor.gw)
		return 0;
	uintptr_t phys = *(const uintptr_t*)(KLIB_MEMBER(5, g_cursor.gw, GameWorld_physics, GW_PHYSICS));
	return PlausiblePtr(phys) ? *(const uintptr_t*)(phys + PHYS_NWORLD) : 0;
}

enum CsLayout { CS_OK, CS_BAD, CS_UNDECIDED };

const int LOCK_UNDECIDED_MAX = 64;

// A Vista+ critical section's LockCount is always negative: -1 at rest, bit 0
// clear while held, 4 less per waiter. Unlocked, it has no owner and no
// recursion; held, the owner is a 32-bit thread id with recursion >= 1. The
// three fields are read without the lock, so a sample taken while the section
// changes hands can disagree with itself: that is undecided, not a bad layout.
// Only a run of undecided samples with no consistent one rejects the address.
static int CriticalSectionLayout(uintptr_t cs)
{
	LONG lockCount = *(const LONG*)(cs + CORE_LOCK_COUNT);
	LONG recursion = *(const LONG*)(cs + CORE_LOCK_RECURSION);
	uintptr_t owner = *(const uintptr_t*)(cs + CORE_LOCK_OWNER);
	if (lockCount >= 0 || recursion < 0 || owner > 0xFFFFFFFFull)
		return CS_BAD;
	bool unlocked = (lockCount & 1) != 0;
	if (unlocked)
		return recursion == 0 && owner == 0 ? CS_OK : CS_UNDECIDED;
	return owner != 0 && recursion >= 1 ? CS_OK : CS_UNDECIDED;
}

bool PhysCoreLockSample(uintptr_t npScene, int* lockCount, int* recursion, unsigned* ownerTid)
{
	CursorState& s = g_cursor;
	if (!s.physxCore || s.lockLayoutRejected || !PlausiblePtr(npScene))
		return false;
	if (s.ownerNpScene == npScene && s.coreLockAddr)
	{
		uintptr_t currentCore = *(const uintptr_t*)(npScene + NPSCENE_CORE);
		if (s.ownerCoreScene == currentCore)
		{
			*lockCount = *(const LONG*)(s.coreLockAddr + CORE_LOCK_COUNT);
			*recursion = *(const LONG*)(s.coreLockAddr + CORE_LOCK_RECURSION);
			*ownerTid = (unsigned)*(const uintptr_t*)(s.coreLockAddr + CORE_LOCK_OWNER);
			return true;
		}
	}
	uintptr_t mb = 0, me = 0;
	uintptr_t npVt = *(const uintptr_t*)npScene;
	if (!PlausiblePtr(npVt) || !ModuleRange((const void*)npVt, &mb, &me) ||
	    mb != (uintptr_t)s.physxCore)
		return false;
	uintptr_t core = *(const uintptr_t*)(npScene + NPSCENE_CORE);
	s.ownerNpScene = npScene;
	s.ownerCoreScene = core;
	s.coreLockAddr = 0;
	if (!PlausiblePtr(core))
		return false;
	uintptr_t coreVt = *(const uintptr_t*)core;
	if (!PlausiblePtr(coreVt) || !ModuleRange((const void*)coreVt, &mb, &me) ||
	    mb != (uintptr_t)s.physxCore)
		return false;
	uintptr_t holder = core + CORE_SCENE_LOCK;
	uintptr_t impl = *(const uintptr_t*)(holder + CORE_LOCK_IMPL_OFF);
	if (!PlausiblePtr(impl))
		return false;
	uintptr_t cs = impl + CORE_LOCK_CS_OFF;
	int layout = CriticalSectionLayout(cs);
	if (layout == CS_UNDECIDED && ++s.lockUndecided >= LOCK_UNDECIDED_MAX)
		layout = CS_BAD;
	if (layout == CS_UNDECIDED)
		return false;
	if (layout == CS_BAD)
	{
		s.lockRejectedAt = cs;
		s.lockLayoutRejected = true;
		return false;
	}
	s.lockUndecided = 0;
	s.coreLockAddr = cs;
	*lockCount = *(const LONG*)(s.coreLockAddr + CORE_LOCK_COUNT);
	*recursion = *(const LONG*)(s.coreLockAddr + CORE_LOCK_RECURSION);
	*ownerTid = (unsigned)*(const uintptr_t*)(s.coreLockAddr + CORE_LOCK_OWNER);
	return true;
}

int PhysOwnerClass(unsigned tid)
{
	if (tid == 0xFFFFFFFFu) return POW_UNKNOWN;
	if (!tid) return POW_NONE;
	if (tid == g_mainThreadId) return POW_MAIN;
	if (tid == g_physThreadId) return POW_PHYS_BACK;
	if (tid == g_aiThreadId) return POW_AI;
	return POW_OTHER;
}

void PhysQueryEnter(int kind, uintptr_t npScene)
{
	CursorState& s = g_cursor;
	s.queryIndex = -1;
	s.queryTag = kind;
	if (!g_cfg.physxDetail || !g_cur.open || g_cur.nphysq >= MAX_PHYS_QUERIES)
		return;
	if (kind < 0 || kind >= PQ_KIND_COUNT)
		return;
	if (kind != PQ_MOUSE_ALL && g_cur.mouseScanDepth <= 0)
		return; // UtilityT::isIndoors has non-cursor callers
	LONGLONG now = Now();
	PhysQuerySample& q = g_cur.physq[g_cur.nphysq];
	memset(&q, 0, sizeof(q));
	q.t = SinceStart(now);
	q.ms = Nan();
	q.ordinal = g_cur.nphysq + 1;
	q.kind = kind;
	q.physRunning = (int)PhysicsRunning();
	LONG activeSeq = InterlockedCompareExchange(&g_physActiveSeq, 0, 0);
	q.runSeq = q.physRunning ? (int)(activeSeq ? activeSeq : g_phys.kickSeq) : 0;
	q.phase = q.physRunning ? (int)InterlockedCompareExchange(&g_physPhase, 0, 0) : PP_IDLE;
	q.lockValid = PhysCoreLockSample(npScene ? npScene : CurrentNpScene(),
	                                 &q.lockCount, &q.recursion, &q.ownerTid) ? 1 : 0;
	q.owner = q.lockValid ? PhysOwnerClass(q.ownerTid) : POW_UNKNOWN;
	s.queryIndex = g_cur.nphysq++;
}

void PhysQueryExit(int kind, LONGLONG ticks)
{
	CursorState& s = g_cursor;
	if (s.queryTag == kind && s.queryIndex >= 0 && s.queryIndex < g_cur.nphysq)
		g_cur.physq[s.queryIndex].ms = TicksToMs(ticks);
	s.queryIndex = -1;
}

} // namespace audit
