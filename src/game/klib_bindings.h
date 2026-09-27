#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void (*KlibLogFn)(const char* message);
// Call once on the startup thread, after platform validation and before
// prologue reads, hooks, patches, workers, or callable binding initialization.
bool InitKlibBindings(uintptr_t base, KlibLogFn log);
// Covered code/global storage uses the exact verified address.
// Uncovered locations remain explicit manual bindings.
uintptr_t KlibAddress(uintptr_t base, uintptr_t legacyRva);

// KenshiLib::GetRealAddress, after following an import thunk the linker put
// in our own image in place of the KenshiLib export (klib_dethunk_policy.h).
// An address outside our image is passed through unchanged. Every
// GetRealAddress lookup goes through here.
intptr_t KlibRealAddressOf(void* function);
// usage: KlibRealAddress(&Class::function), as KenshiLib::GetRealAddress.
template<typename T>
inline intptr_t KlibRealAddress(T function)
{
	static_assert(sizeof(T) >= sizeof(void*), "KlibRealAddress takes a function or member-function pointer");
	return KlibRealAddressOf((void*&)function);
}

// Typed virtual dispatch lives with the real KenshiLib headers. The caller's
// existing null/vtable guards stay in place. No allocator or locking changes.
void KlibProcessZoneContent(void* content);
void KlibPlayerMoveOrder(uintptr_t character, void* building, void* subject, const float* destination);
const float* KlibMovementPosition(void* movement);
bool KlibMovementPathOk(void* movement);
bool KlibMovementPathFailed(void* movement);
bool KlibMovementDestinationReached(void* movement);
void KlibMovementDestination(void* movement, float* destination);
void* KlibBlockAlloc(uintptr_t allocator, int size);
void KlibBlockFree(uintptr_t allocator, void* memory, int size);

typedef void (*KlibMoveOrderFn)(uintptr_t, void*, void*, const float*);
inline void KlibDispatchMoveOrder(KlibMoveOrderFn legacy, uintptr_t character,
                                 void* building, void* subject, const float* destination)
{
	KlibPlayerMoveOrder(character, building, subject, destination);
}

// Character-only lookup; NULL is skipped, never retried through platoons.
void* KlibSelectedCharacter(const void* selectedHand);
