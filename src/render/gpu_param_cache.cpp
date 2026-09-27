#include "render/gpu_param_cache.h"
#include <string.h>

GpuParamCache::GpuParamCache()
{
	Clear();
}

// Both keys are heap addresses in regular strides with idle low bits; the
// mix spreads every input bit over the index.
size_t GpuParamCache::Home(const void* map, const void* name)
{
	unsigned long long x = (unsigned long long)(size_t)map * 0x9E3779B97F4A7C15ull ^ (unsigned long long)(size_t)name;
	x ^= x >> 31;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 29;
	return (size_t)x & (SLOTS - 1);
}

const void* GpuParamCache::Find(const void* map, const MsvcString* name) const
{
	size_t h = Home(map, name);
	for (int i = 0; i < PROBES; ++i)
	{
		const Entry& e = m_slots[(h + i) & (SLOTS - 1)];
		if (e.map != map || e.name != name)
			continue;
		size_t len = name->size;
		if (len != e.len)
			return NULL;
		const char* text = name->Data();
		if (memcmp(text, e.head, len < 16 ? len : 16) != 0)
			return NULL;
		const MsvcString* key = (const MsvcString*)((const char*)e.def - GPC_KEY_TO_DEF);
		if (key->size != len || memcmp(key->Data(), text, len) != 0)
			return NULL;
		return e.def;
	}
	return NULL;
}

void GpuParamCache::Store(const void* map, const MsvcString* name, const void* def)
{
	size_t h = Home(map, name);
	Entry* slot = NULL;
	for (int i = 0; i < PROBES && !slot; ++i)
	{
		Entry& e = m_slots[(h + i) & (SLOTS - 1)];
		if (e.map == map && e.name == name)
			slot = &e;
	}
	for (int i = 0; i < PROBES && !slot; ++i)
	{
		Entry& e = m_slots[(h + i) & (SLOTS - 1)];
		if (!e.map)
			slot = &e;
	}
	if (!slot)
		slot = &m_slots[h];
	size_t len = name->size;
	slot->map = map;
	slot->name = name;
	slot->def = def;
	slot->len = len;
	memset(slot->head, 0, sizeof(slot->head));
	memcpy(slot->head, name->Data(), len < 16 ? len : 16);
}

int GpuParamCache::DropMap(const void* map)
{
	int dropped = 0;
	for (int i = 0; i < SLOTS; ++i)
	{
		if (m_slots[i].map != map)
			continue;
		memset(&m_slots[i], 0, sizeof(m_slots[i]));
		++dropped;
	}
	return dropped;
}

void GpuParamCache::Clear()
{
	memset(m_slots, 0, sizeof(m_slots));
}
