#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fixes/streaming/section_key_scan.h"

#ifdef ZONEOPT_DEBUG
#include "fixes/streaming/section_key_policy.h"
#include "fixes/streaming/section_key_ring.h"
#include "base/core.h"

// The engine indexes the streaming collection's instance array with the top 10
// bits of a packed key and makes no bounds or NULL test. This records, for the
// keys a call is about to use, the array as it stands and where those indices
// land -- which is the difference between a key that is wrong and a slot that
// changed under the call.

// --- the clearance-reset context ---
static const size_t kCtxCollection   = 56;
static const size_t kCtxPendingData  = 80;
static const size_t kCtxPendingCount = 88;
static const size_t kCtxMethod       = 189;

// --- the streaming collection's instance array ---
static const size_t kCollInstancesData  = 32;
static const size_t kCollInstancesSize  = 40;
static const size_t kInstanceInfoStride = 48;

// --- hkArray<int>, as the caller passes it ---
static const size_t kArrayData = 0;
static const size_t kArraySize = 8;

// One key. Everything it needs has already been read into the entry except the
// slot itself, which it reads only when the index is inside the array -- the
// test the engine omits.
static void ClassifyKey(SectionKeyEntry* e, unsigned int key, bool fromCallerArray)
{
	int index = SectionKeyIndex(key);

	++e->keyCount;
	if (fromCallerArray) ++e->keysA; else ++e->keysB;

	if (e->minIndex < 0 || index < e->minIndex)
		e->minIndex = index;
	if (index > e->maxIndex)
	{
		e->maxIndex = index;
		e->maxIndexKey = key;
	}

	if (SectionKeyClassify(key, e->tableSize) == SECTION_KEY_OUT_OF_RANGE)
	{
		if (e->oobCount == 0)
			e->firstOobKey = key;
		++e->oobCount;
		if (fromCallerArray) ++e->oobA; else ++e->oobB;
		return;
	}

	if (!e->tableBase)
		return;
	const unsigned __int64 slot = *(const unsigned __int64*)
		((const unsigned char*)e->tableBase + kInstanceInfoStride * (size_t)index);
	if (!slot)
	{
		if (e->nullSlots == 0)
			e->firstNullIndex = index;
		++e->nullSlots;
	}
}

static void ScanSource(SectionKeyEntry* e, const int* data, int count, bool fromCallerArray)
{
	if (!SectionKeyCountPlausible(count) || (count > 0 && !data))
	{
		e->flags |= SECTION_KEY_FLAG_IMPLAUSIBLE;
		return;
	}
	for (int i = 0; i < count; ++i)
		ClassifyKey(e, (unsigned int)data[i], fromCallerArray);
}

// POD-only and standalone: MSVC 2010 rejects __try in a function that also
// holds an object needing unwinding. The entry's header is stamped before this
// runs, so a fault anywhere inside still leaves a readable, partial entry.
static void ScanClearanceBody(SectionKeyEntry* e, const unsigned char* ctx,
                              const unsigned char* keyArray)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		e->method = *(ctx + kCtxMethod);

		const unsigned char* coll = *(const unsigned char* const*)(ctx + kCtxCollection);
		e->coll = (unsigned __int64)(uintptr_t)coll;
		if (!coll)
		{
			e->flags |= SECTION_KEY_FLAG_NO_COLL;
		}
		else
		{
			e->tableBase = *(const unsigned __int64*)(coll + kCollInstancesData);
			e->tableSize = *(const int*)(coll + kCollInstancesSize);

			if (keyArray)
				ScanSource(e, *(const int* const*)(keyArray + kArrayData),
				           *(const int*)(keyArray + kArraySize), true);
			ScanSource(e, *(const int* const*)(ctx + kCtxPendingData),
			           *(const int*)(ctx + kCtxPendingCount), false);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	if (!ok)
		e->flags |= SECTION_KEY_FLAG_UNREADABLE;
}

static void ScanCutBody(SectionKeyEntry* e, const unsigned char* coll, unsigned int key)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		e->coll = (unsigned __int64)(uintptr_t)coll;
		if (!coll)
		{
			e->flags |= SECTION_KEY_FLAG_NO_COLL;
		}
		else
		{
			e->tableBase = *(const unsigned __int64*)(coll + kCollInstancesData);
			e->tableSize = *(const int*)(coll + kCollInstancesSize);
			ClassifyKey(e, key, true);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	if (!ok)
		e->flags |= SECTION_KEY_FLAG_UNREADABLE;
}

static void Finish(SectionKeyEntry* e)
{
	SectionKeyRingPublish(e);
	SectionKeyRingCount(e->site, e->keyCount, e->oobCount, e->nullSlots,
		(e->flags & SECTION_KEY_FLAG_UNREADABLE) != 0,
		(e->flags & SECTION_KEY_FLAG_IMPLAUSIBLE) != 0);
}

void SectionKeyScanClearance(const void* ctx, const void* keyArray)
{
	if (!ctx)
		return;
	SectionKeyEntry* e = SectionKeyRingReserve(SECTION_KEY_SITE_CLEARANCE);
	ScanClearanceBody(e, (const unsigned char*)ctx, (const unsigned char*)keyArray);
	Finish(e);
}

void SectionKeyScanCut(const void* collection, unsigned int packedKey)
{
	SectionKeyEntry* e = SectionKeyRingReserve(SECTION_KEY_SITE_CUT);
	ScanCutBody(e, (const unsigned char*)collection, packedKey);
	Finish(e);
}

#else

// Nothing of this exists outside a DEV build; plugin_entry.cpp does not name it.
typedef int SectionKeyScanNotInThisBuild;

#endif // ZONEOPT_DEBUG
