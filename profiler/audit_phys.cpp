// audit_phys.cpp - Physics phase and hull timing detours.
// Physics worker records phases; main-thread calls pass through.
// No probe takes a lock, allocates, or logs.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {
void PhysSwitchAt(int next, LONGLONG at)
{
	int current = (int)InterlockedCompareExchange(&g_physPhase, 0, 0);
	if (current >= 0 && current < PP_COUNT && current != PP_IDLE && g_physPhaseStart)
		g_phys.physPhaseTicks[current] += at - g_physPhaseStart;
	g_physPhaseStart = at;
	InterlockedExchange(&g_physPhase, next);
}

void PhysDetailBegin(LONG seq, LONGLONG at)
{
	memset(g_phys.physPhaseTicks, 0, sizeof(g_phys.physPhaseTicks));
	memset(g_phys.physQueued, 0xFF, sizeof(g_phys.physQueued));
	memset(g_phys.physCalls, 0, sizeof(g_phys.physCalls));
	g_phys.physHulls = -1;
	g_physThreadId = GetCurrentThreadId();
	InterlockedExchange(&g_physActiveSeq, seq);
	g_physPhaseStart = at;
	InterlockedExchange(&g_physPhase, PP_BODY_OTHER);
}

void PhysDetailEnd(LONGLONG at)
{
	PhysSwitchAt(PP_IDLE, at);
	InterlockedExchange(&g_physActiveSeq, 0);
}

void PhysCaptureQueues(uintptr_t self)
{
	if (!PlausiblePtr(self))
		return;
	g_phys.physQueued[PO_MAKE]          = *(const int*)(self + PHYS_Q_MAKE);
	g_phys.physQueued[PO_GROUP]         = *(const int*)(self + PHYS_Q_GROUP);
	g_phys.physQueued[PO_IMPULSE]       = *(const int*)(self + PHYS_Q_IMPULSE);
	g_phys.physQueued[PO_HULL_DESTROY]  = *(const int*)(self + PHYS_Q_HULL_DESTROY);
	g_phys.physQueued[PO_ACTOR_DESTROY] = *(const int*)(self + PHYS_Q_ACTOR_DESTROY);
	g_phys.physQueued[PO_TERRAIN]       = *(const int*)(self + PHYS_Q_TERRAIN);
	g_phys.physHulls                    = *(const int*)(self + PHYS_HULLS);
}

int PhysPhaseForTag(int tag)
{
	switch (tag)
	{
	case ST_PHYSLOCK:          return PP_LOCK;
	case ST_PHYSPRE:           return PP_PRE_OTHER;
	case ST_PHYSPOST:          return PP_POST;
	case ST_PHYS_GROUP:        return PP_GROUP;
	case ST_PHYS_ACTOR_DESTROY:return PP_ACTOR_DESTROY;
	case ST_PHYS_TERRAIN:      return PP_TERRAIN;
	case ST_PHYS_FLUSH:        return PP_FLUSH;
	case ST_PHYS_FETCH:        return PP_FETCH;
	default:                   return PP_BODY_OTHER;
	}
}

void PhysProbeEnter(int tag, uintptr_t self)
{
	if (!g_cfg.physxDetail)
		return;
	if (tag == ST_PHYSPRE)
		PhysCaptureQueues(self);
	switch (tag)
	{
	case ST_PHYS_GROUP:         ++g_phys.physCalls[PO_GROUP]; break;
	case ST_PHYS_ACTOR_DESTROY: ++g_phys.physCalls[PO_ACTOR_DESTROY]; break;
	case ST_PHYS_TERRAIN:       ++g_phys.physCalls[PO_TERRAIN]; break;
	default: break;
	}
	PhysSwitchAt(PhysPhaseForTag(tag), Now());
}

void PhysProbeExit(int tag, LONGLONG at)
{
	if (!g_cfg.physxDetail)
		return;
	int next = PP_BODY_OTHER;
	if (tag == ST_PHYSPRE)
		next = PP_SIMULATE;
	else if (tag == ST_PHYS_GROUP || tag == ST_PHYS_ACTOR_DESTROY || tag == ST_PHYS_TERRAIN)
		next = PP_PRE_OTHER;
	else if (tag == ST_PHYS_FETCH)
		next = PP_CONTROLLER; // includes the nullable controller call and branch to physPost
	PhysSwitchAt(next, at);
}

int PhysImplEnter(int phase, int op, bool count)
{
	if (!g_cfg.physxDetail || GetCurrentThreadId() != g_physThreadId ||
	    InterlockedCompareExchange(&g_physActiveSeq, 0, 0) == 0)
		return -1;
	int previous = (int)InterlockedCompareExchange(&g_physPhase, 0, 0);
	PhysSwitchAt(phase, Now());
	if (count && op >= 0 && op < PO_COUNT)
		++g_phys.physCalls[op];
	return previous;
}

void PhysImplExit(int previous)
{
	if (previous >= 0)
		PhysSwitchAt(previous, Now());
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

PhysMake_t      oPhysMakeHull = NULL;
PhysMake_t      oPhysMakeFile = NULL;
PhysMake_t      oPhysMakeScythe = NULL;
PhysFinish_t    oPhysFinishScythe = NULL;
PhysApplyHull_t oPhysApplyHull = NULL;
PhysApplyDoor_t oPhysApplyDoor = NULL;

unsigned char hk_PhysMakeHull(void* self)
{
	int previous = PhysImplEnter(PP_MAKE, PO_MAKE, true);
	unsigned char result = oPhysMakeHull(self);
	PhysImplExit(previous);
	return result;
}

unsigned char hk_PhysMakeFile(void* self)
{
	int previous = PhysImplEnter(PP_MAKE, PO_MAKE, true);
	unsigned char result = oPhysMakeFile(self);
	PhysImplExit(previous);
	return result;
}

unsigned char hk_PhysMakeScythe(void* self)
{
	int previous = PhysImplEnter(PP_MAKE, PO_MAKE, true);
	unsigned char result = oPhysMakeScythe(self);
	PhysImplExit(previous);
	return result;
}

__int64 hk_PhysFinishScythe(void* self, unsigned char inserted)
{
	int previous = PhysImplEnter(PP_MAKE, -1, false);
	__int64 result = oPhysFinishScythe(self, inserted);
	PhysImplExit(previous);
	return result;
}

__int64 hk_PhysApplyHull(void* self)
{
	int previous = PhysImplEnter(PP_HULL_APPLY, PO_HULL_APPLY, true);
	__int64 result = oPhysApplyHull(self);
	PhysImplExit(previous);
	return result;
}

void hk_PhysApplyDoor(void* self)
{
	int previous = PhysImplEnter(PP_HULL_APPLY, PO_HULL_APPLY, true);
	oPhysApplyDoor(self);
	PhysImplExit(previous);
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
