#include "render/upload_shadow.h"
#include <cstdlib>
#include <cstring>

void UploadShadowTable::Configure(int slotCount, size_t maxBytes)
{
	Clear();
	capacity = slotCount;
	byteCap = maxBytes;
}

static void FreeSlots(UploadShadow* slots, int capacity)
{
	for (int i = 0; i < capacity; ++i)
	{
		free(slots[i].bytes);
		free(slots[i].plan);
	}
}

void UploadShadowTable::Clear()
{
	if (slots)
	{
		FreeSlots(slots, capacity);
		free(slots);
	}
	slots = NULL;
	count = 0;
	bytes = 0;
}

void UploadShadowTable::ReclaimIfCrowded()
{
	if (!slots || count <= capacity / 2)
		return;
	FreeSlots(slots, capacity);
	memset(slots, 0, (size_t)capacity * sizeof(UploadShadow));
	count = 0;
	bytes = 0;
}

void UploadShadowTable::InvalidateCopies()
{
	++gen;
	ReclaimIfCrowded();
}

void UploadShadowTable::InvalidateAll()
{
	++gen;
	++planGen;
	ReclaimIfCrowded();
}

static int SlotOf(const UploadShadow* slots, int capacity, const void* buffer)
{
	size_t h = (size_t)buffer;
	h ^= h >> 17;
	h *= (size_t)0x9E3779B97F4A7C15ULL;
	int i = (int)(h >> 40) & (capacity - 1);
	while (slots[i].buffer && slots[i].buffer != buffer)
		i = (i + 1) & (capacity - 1);
	return i;
}

static void DropPlan(UploadShadow& s)
{
	s.planValid = false;
	s.planRefused = false;
}

UploadShadow* UploadShadowTable::Acquire(const void* buffer, const void* owner, size_t size)
{
	if (!buffer || capacity <= 0)
		return NULL;
	if (!slots)
	{
		slots = (UploadShadow*)calloc((size_t)capacity, sizeof(UploadShadow));
		if (!slots)
			return NULL;
	}
	UploadShadow& s = slots[SlotOf(slots, capacity, buffer)];
	if (!s.buffer)
	{
		// Fill stops at 3/4 so every probe chain ends at an empty slot.
		if (count >= capacity / 4 * 3 || size > byteCap - bytes)
			return NULL;
		unsigned char* copy = (unsigned char*)calloc(1, size);
		if (!copy)
			return NULL;
		s.buffer = buffer;
		s.owner = owner;
		s.bytes = copy;
		s.size = size;
		s.valid = false;
		s.gen = gen;
		s.planGen = planGen;
		DropPlan(s);
		++count;
		bytes += size;
		return &s;
	}
	if (s.owner != owner || s.size != size)
	{
		// A reused address: the new buffer's size may differ, and the old
		// bytes and plan say nothing about it.
		if (size > s.size && size - s.size > byteCap - bytes)
			return NULL;
		unsigned char* copy = (unsigned char*)realloc(s.bytes, size);
		if (!copy)
			return NULL;
		bytes = bytes - s.size + size;
		s.bytes = copy;
		s.size = size;
		s.owner = owner;
		s.valid = false;
		DropPlan(s);
	}
	if (s.gen != gen)
	{
		s.gen = gen;
		s.valid = false;
	}
	if (s.planGen != planGen)
	{
		s.planGen = planGen;
		DropPlan(s);
	}
	return &s;
}

bool UploadShadowTable::ReservePlan(UploadShadow* s, int steps)
{
	s->valid = false;
	DropPlan(*s);
	s->planCount = 0;
	if (steps < 0)
		return false;
	if (steps <= s->planCap)
		return true;
	size_t have = (size_t)s->planCap * sizeof(UploadPlanStep);
	size_t want = (size_t)steps * sizeof(UploadPlanStep);
	if (want - have > byteCap - bytes)
		return false;
	UploadPlanStep* plan = (UploadPlanStep*)realloc(s->plan, want);
	if (!plan)
		return false;
	bytes += want - have;
	s->plan = plan;
	s->planCap = steps;
	return true;
}

bool UploadShadowUpdate(UploadShadow* s, size_t offset, const void* src, size_t n, bool* differs)
{
	if (offset > s->size || n > s->size - offset)
		return false;
	if (memcmp(s->bytes + offset, src, n) != 0)
	{
		*differs = true;
		memcpy(s->bytes + offset, src, n);
	}
	return true;
}
