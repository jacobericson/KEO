// audit_threads.cpp - Worker body and character update timing.
// AI, physics and birds workers write their slots; inline bodies may run on main.
// No probe takes a lock, allocates, or logs.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {
void RunWorkerBody(ThreadSlot& s, ThreadBody_t orig, void* self, float ft, bool inf, bool isAi)
{
	LONG seq = s.kickSeq;
	s.runSeq = seq;
	s.lock = s.pre = s.post = 0;
	bool isPhys = &s == &g_phys;
	if (isAi)
	{
		g_aiThreadId = GetCurrentThreadId();
		s.zone = s.content = s.factions = s.vis = 0;
		s.env = s.forced = 0;
		s.tu = s.tu4 = s.tup = s.tuMax = 0;
		s.l1Task = s.l1Move = s.l1Flush = s.l1Anim = 0;
		s.ntask = 0;
		s.visCalls = 0;
		s.l1 = *(const int*)((const char*)self + AI_LIST_TU);
		s.l4 = *(const int*)((const char*)self + AI_LIST_TU4);
		s.lp = *(const int*)((const char*)self + AI_LIST_TUP);
	}
	s.bodyIn = Now();
	if (isPhys && g_cfg.physxDetail)
		PhysDetailBegin(seq, s.bodyIn);
	orig(self, ft, inf);
	s.bodyOut = Now();
	if (isPhys && g_cfg.physxDetail)
		PhysDetailEnd(s.bodyOut);
	_WriteBarrier();
	InterlockedExchange(&s.doneSeq, seq);
}

void hk_AiBody(void* self, float ft, bool inf)
{
	if (IsMain())
	{
		// characterMultithreading off: the body runs inline on the main thread.
		LONGLONG t0 = Now();
		oAiBody(self, ft, inf);
		float ms = TicksToMs(Now() - t0);
		if (g_cur.open)
		{
			g_cur.flags |= F_AISYNC;
			g_cur.aiRun = IsNan(g_cur.aiRun) ? ms : g_cur.aiRun + ms;
		}
		return;
	}
	RunWorkerBody(g_ai, oAiBody, self, ft, inf, true);
}

void hk_PhysBody(void* self, float ft, bool inf)
{
	if (IsMain())
	{
		oPhysBody(self, ft, inf);
		return;
	}
	RunWorkerBody(g_phys, oPhysBody, self, ft, inf, false);
}

void hk_BirdsBody(void* self, float ft, bool inf)
{
	if (IsMain())
	{
		oBirdsBody(self, ft, inf);
		return;
	}
	RunWorkerBody(g_birds, oBirdsBody, self, ft, inf, false);
}

// ---- Inside list 1 (AI thread only; inline runs on the main thread pass) ---

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
BodyUpdate_t oBodyUpdate = NULL;
MoveUpdate_t oMoveUpdate = NULL;

inline bool OnAiThread()
{
	DWORD ai = g_aiThreadId;
	return ai != 0 && GetCurrentThreadId() == ai;
}

void TaskAdd(ThreadSlot& s, const void* vt, LONGLONG d)
{
	for (int i = 0; i < s.ntask; ++i)
	{
		if (s.taskVt[i] == vt)
		{
			s.taskTicks[i] += d;
			++s.taskCalls[i];
			return;
		}
	}
	if (s.ntask < ThreadSlot::MAX_TASKS)
	{
		int i = s.ntask++;
		s.taskVt[i]    = vt;
		s.taskTicks[i] = d;
		s.taskCalls[i] = 1;
	}
}

// CharBody::update runs the character's current task (tail call into the
// Task_* object's update), so its time is the task-execution share of list 1.
// The task's vtable is read before the call: the call can delete the task.
unsigned __int64 hk_BodyUpdate(void* body, float dt)
{
	if (!OnAiThread())
		return oBodyUpdate(body, dt);
	// The game reads the task only once the character has an active platoon
	// (vt+0x58 = 0x5C5BC0 checks Character+0x658): read it under the same test.
	const void* vt = NULL;
	uintptr_t ch = *(const uintptr_t*)(KLIB_MEMBER(5, (const char*)body, CharBody_character, CHARBODY_CHAR));
	if (PlausiblePtr(ch) && *(const uintptr_t*)(KLIB_MEMBER(5, ch, Character_platoon, CHAR_ACTIVE_PLATOON)) != 0)
	{
		uintptr_t task = *(const uintptr_t*)(KLIB_MEMBER(5, (const char*)body, CharBody_currentAction, CHARBODY_TASK));
		if (PlausiblePtr(task))
		{
			uintptr_t v = *(const uintptr_t*)task;
			if (v >= g_base && v < g_exeEnd)
				vt = (const void*)v;
		}
	}
	LONGLONG t0 = Now();
	unsigned __int64 r = oBodyUpdate(body, dt);
	LONGLONG d = Now() - t0;
	g_ai.l1Task += d;
	TaskAdd(g_ai, vt, d);
	return r;
}

void hk_MoveUpdate(void* movement, float dt)
{
	if (!OnAiThread())
	{
		oMoveUpdate(movement, dt);
		return;
	}
	LONGLONG t0 = Now();
	oMoveUpdate(movement, dt);
	g_ai.l1Move += Now() - t0;
}

void hk_PhysUT(void* self)
{
	bool main = IsMain();
	if (main)
		Hulls_BeforeUpdateUT(self);   // updateUT opens with the queue flushes
	if (!main || !g_cur.open)
	{
		oPhysUT(self);
		return;
	}
	LONGLONG t0 = Now();
	oPhysUT(self);
	g_cur.ml[M_ML_PHYSUT - ML_FIRST] += Now() - t0;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
