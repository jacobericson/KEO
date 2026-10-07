// scene_lever_policy.cpp - the scene switches' pure rules (scene_lever_policy.h).
#include "render/scene_lever_policy.h"
#include <limits.h>
#include <string.h>

static uintptr_t Ptr(const unsigned char* p, size_t off)
{
	uintptr_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static size_t Size(const unsigned char* p, size_t off)
{
	size_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

bool CullSlotIsBase(uintptr_t slot, uintptr_t baseCull, CullThunkCache* cache, const CullReaders* rd)
{
	if (!slot || !baseCull)
		return false;
	if (slot == baseCull)
		return true;
	if (cache)
		for (size_t i = 0; i < cache->count && i < CULL_THUNK_CACHE; ++i)
			if (cache->slot[i] == slot)
				return true;
	if (!rd || !rd->bytes || !rd->ptr)
		return false;
	unsigned char op[3];
	if (!rd->bytes(slot, op, sizeof(op)))
		return false;
	size_t len;
	if (op[0] == 0xFF && op[1] == 0x25)
		len = 6;
	else if (op[0] == 0x48 && op[1] == 0xFF && op[2] == 0x25)
		len = 7;
	else
		return false;
	unsigned char d[4];
	if (!rd->bytes(slot + len - 4, d, sizeof(d)))
		return false;
	int32_t disp;
	memcpy(&disp, d, sizeof(disp));
	uintptr_t target;
	if (!rd->ptr(slot + len + (uintptr_t)(intptr_t)disp, &target) || target != baseCull)
		return false;
	if (cache && cache->count < CULL_THUNK_CACHE)
		cache->slot[cache->count++] = slot;
	return true;
}

bool ForkR8Idle(const unsigned char* sm, uintptr_t baseCull, CullThunkCache* cache, const CullReaders* rd)
{
	if (!baseCull)
		return false;
	const size_t n = Size(sm, SM_VISIBLE_LIST_COUNT);
	if (n == 0)
		return true;
	const uintptr_t lists = Ptr(sm, SM_VISIBLE_LISTS);
	if (n > FORK_MAX_LISTS || !lists)
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const unsigned char* list = (const unsigned char*)(lists + i * VISIBLE_LIST_STRIDE);
		const size_t count = Size(list, VISIBLE_LIST_SIZE);
		if (count == 0)
			continue;
		const uintptr_t data = Ptr(list, 0);
		if (count > FORK_MAX_OBJECTS || !data)
			return false;
		for (size_t k = 0; k < count; ++k)
		{
			const uintptr_t obj = ((const uintptr_t*)data)[k];
			if (!obj)
				return false;
			const uintptr_t vtbl = *(const uintptr_t*)obj;
			if (!vtbl)
				return false;
			const uintptr_t slot = ((const uintptr_t*)vtbl)[VT_BATCH_CULL / 8];
			if (!CullSlotIsBase(slot, baseCull, cache, rd))
				return false;
		}
	}
	return true;
}

// The manager pointers in [begin, end), or false when the range is not one.
static bool ManagerRange(const unsigned char* sm, size_t beginOff, size_t endOff,
                         const uintptr_t** first, size_t* count)
{
	const uintptr_t b = Ptr(sm, beginOff);
	const uintptr_t e = Ptr(sm, endOff);
	*first = (const uintptr_t*)b;
	*count = 0;
	if (b == e)
		return true;
	if (!b || e < b || (e - b) % 8 || (e - b) / 8 > FORK_MAX_MANAGERS)
		return false;
	*count = (size_t)((e - b) / 8);
	return true;
}

bool ForkR1Idle(const unsigned char* sm)
{
	const uintptr_t* mgrs;
	size_t n;
	if (!ManagerRange(sm, SM_SKEL_MGRS_BEGIN, SM_SKEL_MGRS_END, &mgrs, &n))
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const uintptr_t mgr = mgrs[i];
		if (!mgr)
			return false;
		const uintptr_t head = Ptr((const unsigned char*)mgr, 0);
		if (!head)
			return false;
		if (Ptr((const unsigned char*)head, 0) != head)
			return false;
	}
	return true;
}

bool ForkR7Idle(const unsigned char* sm)
{
	const uintptr_t* mgrs;
	size_t n;
	if (!ManagerRange(sm, SM_INST_MGRS_BEGIN, SM_INST_MGRS_END, &mgrs, &n))
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const unsigned char* mgr = (const unsigned char*)mgrs[i];
		if (!mgr)
			return false;
		if (Ptr(mgr, IM_DYNAMIC_BEGIN) != Ptr(mgr, IM_DYNAMIC_END)
		    || Ptr(mgr, IM_DIRTY_BEGIN) != Ptr(mgr, IM_DIRTY_END))
			return false;
	}
	return true;
}

ForkAction ForkFire(ForkPairState* s, bool on, bool mainThread, bool idle)
{
	if (!mainThread)
		return FORK_CALL;
	if (s->pending)
	{
		s->pending = false;
		return FORK_CALL_ODD;
	}
	if (!on || !s->live || !idle)
		return FORK_CALL;
	s->pending = true;
	return FORK_SKIP;
}

ForkAction ForkWait(ForkPairState* s, bool mainThread)
{
	if (mainThread && s->pending)
	{
		s->pending = false;
		return FORK_SKIP;
	}
	return FORK_CALL;
}

size_t BuildForkStub(unsigned char* out, size_t cap, ForkReg reg, int site, unsigned long long entry)
{
	if (cap < FORK_STUB_BYTES)
		return 0;
	static const unsigned char kMovRsi[3] = { 0x48, 0x89, 0xF2 };   // mov rdx, rsi
	static const unsigned char kMovR12[3] = { 0x4C, 0x89, 0xE2 };   // mov rdx, r12
	static const unsigned char kMovRdi[3] = { 0x48, 0x89, 0xFA };   // mov rdx, rdi
	const unsigned char* mov;
	switch (reg)
	{
	case FORK_REG_RSI: mov = kMovRsi; break;
	case FORK_REG_R12: mov = kMovR12; break;
	case FORK_REG_RDI: mov = kMovRdi; break;
	default: return 0;
	}
	size_t n = 0;
	for (int i = 0; i < 3; ++i)
		out[n++] = mov[i];
	out[n++] = 0x41;                                                 // mov r8d, imm32
	out[n++] = 0xB8;
	const unsigned int s = (unsigned int)site;
	for (int i = 0; i < 4; ++i)
		out[n++] = (unsigned char)(s >> (8 * i));
	out[n++] = 0x48;                                                 // mov rax, imm64
	out[n++] = 0xB8;
	for (int i = 0; i < 8; ++i)
		out[n++] = (unsigned char)(entry >> (8 * i));
	out[n++] = 0xFF;                                                 // jmp rax
	out[n++] = 0xE0;
	return n;
}

bool Rel32To(unsigned long long next, unsigned long long target, int32_t* rel)
{
	const long long d = (long long)(target - next);
	if (d < (long long)INT_MIN || d > (long long)INT_MAX)
		return false;
	*rel = (int32_t)d;
	return true;
}

bool ForkSiteMatches(const unsigned char* window, const unsigned char* expect, size_t len,
                     unsigned long long site, unsigned long long target)
{
	if (len < 5 || memcmp(window, expect, len) != 0 || window[len - 5] != 0xE8)
		return false;
	int32_t rel;
	memcpy(&rel, window + len - 4, sizeof(rel));
	return site + 5 + (unsigned long long)(long long)rel == target;
}

InstUpload InstUploadDecide(bool mainThread, const unsigned char* batch)
{
	if (!mainThread)
		return INST_OFF_MAIN;
	if (!batch)
		return INST_RUN;
	const uintptr_t mgr = Ptr(batch, BATCH_SCENE_MANAGER);
	if (!mgr)
		return INST_RUN;
	int threaded;
	memcpy(&threaded, (const unsigned char*)mgr + SM_THREADED_INSTANCING, sizeof(threaded));
	if (threaded == 0)
		return INST_UNTHREADED;
	if (Size(batch, BATCH_CULLED_COUNT) == 0)
		return INST_EMPTY;
	return INST_RUN;
}

RqClearAction RqClearStep(int mode, int applied)
{
	const bool on = mode == 1;
	if (on && applied != 1)
		return RQ_SET_ON;
	if (!on && applied == 1)
		return RQ_RESTORE;
	return RQ_NONE;
}
