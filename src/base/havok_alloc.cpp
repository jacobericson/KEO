// havok_alloc.cpp - Allocation through the calling thread's Havok heap.
// Any registered Havok thread; no mod lock is taken.

#include "base/core.h"
#include "game/klib_bindings.h"

// =========================================================================
// Havok TLS heap allocator (used by navmesh_cache + pathfind_diag)
// =========================================================================

// Forward-declared here; gameBase and RVA_HAVOK_TLS_INDEX come from game.h,
// which this Layer 0 unit does not include (core.h has no game knowledge).
// Instead, these are set by game.cpp's InitGameBindings and stored here.
static uintptr_t s_gameBase = 0;
static size_t s_havokTlsRva = 0;

void SetHavokTlsParams(uintptr_t base, size_t rva)
{
	s_gameBase = base;
	s_havokTlsRva = rva;
}

void* HavokTlsAlloc(size_t size)
{
	if (!s_gameBase || !s_havokTlsRva)
		return NULL;
	DWORD tlsIdx = *(DWORD*)(s_gameBase + s_havokTlsRva);
	uintptr_t* allocs = (uintptr_t*)TlsGetValue(tlsIdx);
	if (!allocs) return NULL;
	uintptr_t allocObj = allocs[11];
	if (!allocObj) return NULL;
	return KlibBlockAlloc(allocObj, (int)size);
}

void HavokTlsFree(void* ptr, size_t size)
{
	if (!ptr || !s_gameBase || !s_havokTlsRva)
		return;
	DWORD tlsIdx = *(DWORD*)(s_gameBase + s_havokTlsRva);
	uintptr_t* allocs = (uintptr_t*)TlsGetValue(tlsIdx);
	if (!allocs) return;
	uintptr_t allocObj = allocs[11];
	if (!allocObj) return;
	KlibBlockFree(allocObj, ptr, (int)size);
}
