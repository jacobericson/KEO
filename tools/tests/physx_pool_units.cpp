// Host unit tests for the lock-free pieces of the loadPhysXResource
// pass-through: caller classification, the bounded distinct sets, and the
// formatting rule that keeps "never sampled" apart from "zero".

#include "diag/physx_pool_sets.h"
#include <cstdio>
#include <cstring>

#include "check.h"


static void CheckStr(const char* got, const char* want, const char* what)
{
	if (strcmp(got, want) != 0)
	{
		printf("  FAIL: %s (got \"%s\", wanted \"%s\")\n", what, got, want);
		Check(false, what);
	}
}


static void TestClassify()
{
	// The four known call sites, at their first and last byte.
	Check(PhysXPoolClassifyCaller(0x7E49E0, true) == PXP_ROLE_PHYS, "loadPhysXFile start");
	Check(PhysXPoolClassifyCaller(0x7E4C49, true) == PXP_ROLE_PHYS, "loadPhysXFile end");
	Check(PhysXPoolClassifyCaller(0x7E4C50, true) == PXP_ROLE_TRIG, "trigger start");
	Check(PhysXPoolClassifyCaller(0x7E4FEC, true) == PXP_ROLE_TRIG, "trigger end");
	Check(PhysXPoolClassifyCaller(0x3C7870, true) == PXP_ROLE_NAV,  "interior mesh start");
	Check(PhysXPoolClassifyCaller(0x3C850F, true) == PXP_ROLE_NAV,  "interior mesh end");
	Check(PhysXPoolClassifyCaller(0x7E5000, true) == PXP_ROLE_XML,  "xml convert start");
	Check(PhysXPoolClassifyCaller(0x7E53D5, true) == PXP_ROLE_XML,  "xml convert end");

	// The two physics callers are adjacent; the boundary must not smear.
	Check(PhysXPoolClassifyCaller(0x7E4C4A, true) == PXP_ROLE_EXE_OTHER,
		"gap between loadPhysXFile and the trigger variant");

	Check(PhysXPoolClassifyCaller(0x400000, true) == PXP_ROLE_EXE_OTHER, "unknown exe caller");

	// Outside the executable can only mean another module's detour wraps ours,
	// and it must win over any RVA arithmetic.
	Check(PhysXPoolClassifyCaller(0x7E49E0, false) == PXP_ROLE_FOREIGN, "foreign beats a matching rva");
	Check(PhysXPoolClassifyCaller(0, false) == PXP_ROLE_FOREIGN, "foreign caller");
}

static void TestRoleMask()
{
	char buf[64];
	PhysXPoolFormatRoleMask(buf, sizeof(buf), 0);
	CheckStr(buf, "-", "empty mask");

	PhysXPoolFormatRoleMask(buf, sizeof(buf), (1UL << PXP_ROLE_PHYS));
	CheckStr(buf, "phys", "one role");

	// The reading the whole instrument exists for.
	PhysXPoolFormatRoleMask(buf, sizeof(buf),
		(1UL << PXP_ROLE_PHYS) | (1UL << PXP_ROLE_NAV));
	CheckStr(buf, "phys|nav", "the physics/navmesh pair");

	// Two navmesh-side entrants cannot be told from one by a mask alone, so
	// the mask must never be the only evidence: nav alone at depth >= 2 is a
	// distinct, reportable state.
	PhysXPoolFormatRoleMask(buf, sizeof(buf), (1UL << PXP_ROLE_NAV));
	CheckStr(buf, "nav", "navmesh alone");

	// A buffer too small stops on a whole tag rather than writing half of one.
	char small[6];
	PhysXPoolFormatRoleMask(small, sizeof(small),
		(1UL << PXP_ROLE_PHYS) | (1UL << PXP_ROLE_NAV));
	CheckStr(small, "phys", "truncation keeps whole tags");
}

static void TestSet()
{
	PhysXPoolSet set;
	PhysXPoolSetReset(&set);
	Check(set.distinct == 0 && set.samples == 0 && set.overflow == 0, "reset clears");

	Check(PhysXPoolSetInsert(&set, 0x1111) == PXP_INSERT_NEW, "first insert is new");
	Check(PhysXPoolSetInsert(&set, 0x1111) == PXP_INSERT_PRESENT, "repeat insert is present");
	Check(set.distinct == 1, "one distinct value");
	Check(set.samples == 2, "two samples");

	Check(PhysXPoolSetInsert(&set, 0x2222) == PXP_INSERT_NEW, "second value");
	Check(set.distinct == 2, "two distinct values");

	// A key that hashes to the empty marker must still be counted.
	PhysXPoolSetReset(&set);
	Check(PhysXPoolSetInsert(&set, 0) == PXP_INSERT_NEW, "zero key inserts");
	Check(PhysXPoolSetInsert(&set, 0) == PXP_INSERT_PRESENT, "zero key dedupes");
	Check(set.distinct == 1, "zero key counted once");

	// Filling the table must raise overflow rather than quietly miscount.
	PhysXPoolSetReset(&set);
	for (int i = 1; i <= PXP_SET_SLOTS; ++i)
		PhysXPoolSetInsert(&set, (unsigned __int64)i * 0x9E3779B97F4A7C15ULL);
	Check(set.distinct == PXP_SET_SLOTS, "table fills to capacity");
	Check(set.overflow == 0, "no overflow at exactly capacity");
	PhysXPoolSetInsert(&set, 0xDEADBEEFCAFEULL);
	Check(set.overflow == 1, "one past capacity overflows");
}

static void TestHash()
{
	// Distinct scales must not collide, because the engine's interned name is
	// built from their decimal spelling: two scales are two pooled strings.
	unsigned __int64 unit  = PhysXPoolHashScale(1.0f, 1.0f, 1.0f, PXP_HASH_SEED);
	unsigned __int64 tiny  = PhysXPoolHashScale(1.0f, 1.0f, 1.0000001f, PXP_HASH_SEED);
	unsigned __int64 swap  = PhysXPoolHashScale(2.0f, 1.0f, 1.0f, PXP_HASH_SEED);
	unsigned __int64 swap2 = PhysXPoolHashScale(1.0f, 2.0f, 1.0f, PXP_HASH_SEED);
	Check(unit != tiny, "a one-bit scale difference is a different key");
	Check(swap != swap2, "scale order matters");

	// The same scale under two resources is two pair keys, and the same
	// resource under two scales likewise -- that is what separates "many
	// assets at unit scale" from "few assets at many scales".
	unsigned __int64 resA = PhysXPoolHashBytes("a.physx", 7, PXP_HASH_SEED);
	unsigned __int64 resB = PhysXPoolHashBytes("b.physx", 7, PXP_HASH_SEED);
	Check(resA != resB, "distinct resources hash apart");
	Check(PhysXPoolHashScale(2.0f, 2.0f, 2.0f, resA)
	   != PhysXPoolHashScale(2.0f, 2.0f, 2.0f, resB), "pair key carries the resource");
	Check(PhysXPoolHashScale(2.0f, 2.0f, 2.0f, resA)
	   != PhysXPoolHashScale(3.0f, 3.0f, 3.0f, resA), "pair key carries the scale");

	// The resource key includes the type argument, which is part of the cache
	// key the engine and RE_Kenshi both use.
	Check(PhysXPoolHashU32(0, PXP_HASH_SEED) != PhysXPoolHashU32(1, PXP_HASH_SEED),
		"type participates");
}

static void TestFormatting()
{
	char buf[32];
	PhysXPoolSet set;
	PhysXPoolSetReset(&set);

	// Nothing offered: "?", never "0". Zero is a real reading here.
	PhysXPoolFormatSet(buf, sizeof(buf), &set);
	CheckStr(buf, "?", "never sampled prints ?");

	PhysXPoolSetInsert(&set, 7);
	PhysXPoolFormatSet(buf, sizeof(buf), &set);
	CheckStr(buf, "1", "one distinct value");

	set.overflow = 1;
	PhysXPoolFormatSet(buf, sizeof(buf), &set);
	CheckStr(buf, "1+", "overflow is marked, and is not 0 or ?");

	PhysXPoolFormatCount(buf, sizeof(buf), 0, true);
	CheckStr(buf, "0", "a sampled zero prints 0");
	PhysXPoolFormatCount(buf, sizeof(buf), 0, false);
	CheckStr(buf, "?", "an unsampled zero prints ?");
	PhysXPoolFormatCount(buf, sizeof(buf), 3, true);
	CheckStr(buf, "3", "a sampled value prints itself");
}

int main()
{
	TestClassify();
	TestRoleMask();
	TestSet();
	TestHash();
	TestFormatting();

	return CheckExit("physx_pool_units");
}
