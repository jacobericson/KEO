// nm_building_hash.cpp - guarded building-content key walk.
// NavMesh background thread and workers, outside every mod lock; hash inputs stay bit-identical.

#include "navmesh/cache/nm_cache_core.h"
#include "navmesh/cache/nm_key_hash.h"


unsigned int HashAABB(const float* aabb6)
{
	return NmAabbHash(aabb6);
}

volatile long nmHashRaceCount = 0;

// Order-independent building hash: per-building FNV-1a of (position, rotation,
// stringID), summed across all buildings (commutative). Position is the whole
// Vector3 at +0x48, so elevation counts, and rotation is the quaternion at
// +0xB0. Do NOT use hand.index — runtime-assigned.
//
// The walk over one content's things list. Called only from inside the __try
// of BuildingHashGuarded, so a fault anywhere in it is caught there.
static unsigned int BuildingHashWalk(int tCount, uintptr_t* stuffPtr)
{
	unsigned int totalHash = 0;
	if (!stuffPtr || tCount <= 0 || tCount >= 10000) return totalHash;

	for (int t = 0; t < tCount; ++t)
	{
		uintptr_t obj = (uintptr_t)stuffPtr[t];
		if (!obj) continue;
		int handType = *(int*)(KLIB_MEMBER(4, obj, RootObjectBase_handle_type, 0x60));
		if (handType != 0) continue;  // BUILDING only

		// RootObjectBase::pos is an Ogre::Vector3 at +0x48 (x +0x48, y +0x4C,
		// z +0x50) and RootObject::rot an Ogre::Quaternion at +0xB0, 16 bytes
		// (KenshiLib RootObjectBase.h / RootObject.h; getOrientation 0xD1EC0
		// copies the four dwords at +0xB0). Buildings derive from RootObject,
		// whose own members start at +0xC0, so both are in range for the
		// handType == 0 objects this loop keeps. Elevation and rotation change
		// the generated mesh, so they belong in the key.
		const unsigned char* pos = (const unsigned char*)(KLIB_MEMBER(4, obj, RootObjectBase_pos, 0x48));   // pos x, y, z
		const unsigned char* rot = (const unsigned char*)(KLIB_MEMBER(4, obj, RootObject_rot, 0xB0));      // rot quaternion

		// Hash full stringID content (GameData+0x58, MSVC 2010 std::string)
		const unsigned char* sid = NULL;
		size_t len = 0;
		uintptr_t gameData = *(uintptr_t*)(KLIB_MEMBER(4, obj, RootObjectBase_data, 0x40));
		if (gameData)
		{
			uintptr_t strObj = KLIB_MEMBER(4, gameData, GameData_stringID, 0x58);
			size_t strLen = *(size_t*)(KLIB_MEMBER(4, strObj, StdString_size, 16));
			size_t res = *(size_t*)(KLIB_MEMBER(4, strObj, StdString_capacity, 24));
			const unsigned char* strData;
			if (res < 16)
				strData = (const unsigned char*)KLIB_MEMBER(4, strObj, StdString_buffer, 0);
			else
				strData = *(const unsigned char**)KLIB_MEMBER(4, strObj, StdString_pointer, 0);
			if (strData && strLen > 0 && strLen < 256)
			{
				sid = strData;
				len = strLen;
			}
		}

		totalHash += NmBuildingHashOf(pos, rot, sid, len);
	}
	return totalHash;
}

// Standalone and POD-only: MSVC 2010 rejects __try in a function that also
// holds objects needing unwinding. The before/after reads go through volatile
// pointers so the compiler cannot fold the second read into the first; x64
// keeps loads in program order, so "after" really is after the walk.
//
// Order of the "after" reads: the things count and buffer first, zone+0 last,
// so the last thing read is the pointer the game NULLs last. A walk that ends
// in the few instructions between the content's delete and that NULL reads
// equal values from freed memory and passes; ZoneContentUnchanged at the store
// then sees zone+0 NULL (the unload has long finished by the time a 1-2 s
// generation completes) and refuses the store.
static bool BuildingHashGuarded(uintptr_t jobZone, unsigned int* hashOut, uintptr_t* contentOut)
{
	bool ok = false;
	unsigned int hash = 0;
	uintptr_t c0 = 0;

	GuardEnter();
	__try
	{
		volatile uintptr_t* contentSlot =
			(volatile uintptr_t*)KLIB_MEMBER(4, jobZone, ZoneMap_mapContent, OFF_ZONE_CONTENT);
		c0 = *contentSlot;
		if (c0)
		{
			volatile int* countSlot =
				(volatile int*)KLIB_MEMBER(4, c0, RootObjectContainer_things_count, OFF_ZMC_THINGS_COUNT);
			uintptr_t* volatile* stuffSlot =
				(uintptr_t* volatile*)KLIB_MEMBER(4, c0, RootObjectContainer_things_stuff, 96);

			int        count0 = *countSlot;
			uintptr_t* stuff0 = *stuffSlot;

			hash = BuildingHashWalk(count0, stuff0);

			int        count1 = *countSlot;
			uintptr_t* stuff1 = *stuffSlot;
			uintptr_t  c1     = *contentSlot;

			ok = (c1 == c0) && (count1 == count0) && (stuff1 == stuff0);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();

	*hashOut    = ok ? hash : 0;
	*contentOut = ok ? c0 : 0;
	return ok;
}

bool ComputeBuildingHashChecked(uintptr_t jobZone, unsigned int* hashOut, uintptr_t* contentOut)
{
	*hashOut = 0;
	*contentOut = 0;
	if (!jobZone || !BuildingHashGuarded(jobZone, hashOut, contentOut))
	{
		InterlockedIncrement(&nmHashRaceCount);
		return false;
	}
	return true;
}

bool ZoneContentUnchanged(uintptr_t jobZone, uintptr_t content)
{
	if (!jobZone || !content) return false;
	return *(volatile uintptr_t*)KLIB_MEMBER(4, jobZone, ZoneMap_mapContent, OFF_ZONE_CONTENT) == content;
}

// POD-only, same __try rule as above.
int SafeZoneThingsCount(uintptr_t jobZone)
{
	if (!jobZone) return -1;
	int count = -1;
	GuardEnter();
	__try
	{
		uintptr_t content = *(volatile uintptr_t*)KLIB_MEMBER(4, jobZone, ZoneMap_mapContent, OFF_ZONE_CONTENT);
		if (content)
			count = *(volatile int*)KLIB_MEMBER(4, content, RootObjectContainer_things_count, OFF_ZMC_THINGS_COUNT);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		count = -1;
	}
	GuardLeave();
	// A negative count can only be garbage (freed content). Never pass one on:
	// the L2-miss log reads -2 as its "write entry" sentinel.
	return count < 0 ? -1 : count;
}
