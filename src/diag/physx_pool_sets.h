#ifndef KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_SETS_H
#define KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_SETS_H

#include <cstddef>

// Lock-free pieces of the loadPhysXResource pass-through, kept apart from the
// hook so they can be exercised without a live process: caller classification,
// the bounded distinct-value sets and the formatting rule that keeps "absent"
// distinguishable from "zero".

// Which caller reached loadPhysXResource, decided by the return address rather
// than by thread id. Thread identity would put the one entrant the readout
// exists to name into an "unknown" bucket; the four call sites are all direct
// and all known, so the return address names the role exactly.
enum PhysXPoolRole
{
	PXP_ROLE_PHYS = 0,   // PhysicsActual::loadPhysXFile
	PXP_ROLE_TRIG,       // PhysicsActual::loadPhysXFileAsATrigger
	PXP_ROLE_NAV,        // NavMeshGenerator::generateInteriorMesh
	PXP_ROLE_XML,        // PhysicsActual::convertXMLToBin
	PXP_ROLE_EXE_OTHER,  // inside the executable, none of the four above
	PXP_ROLE_FOREIGN,    // outside the executable: another module called us
	PXP_ROLE_COUNT
};

// Short tag for a role, for the mask field ("phys|nav").
const char* PhysXPoolRoleName(int role);

// `retRva` is the return address expressed as an executable RVA; `insideExe`
// says whether the address was inside the executable image at all. An address
// outside it means our detour was entered from another module, which is only
// possible when someone else's detour wraps ours -- so FOREIGN is a statement
// about hook order, not an unknown caller.
int PhysXPoolClassifyCaller(unsigned __int64 retRva, bool insideExe);

// Fills `out` with the set bits as role tags joined by '|', or "-" when no bit
// is set. Returns the number of characters written (excluding the NUL).
size_t PhysXPoolFormatRoleMask(char* out, size_t cap, unsigned long mask);


// ---------------------------------------------------------------------------
// Bounded distinct-value set
// ---------------------------------------------------------------------------
//
// A fixed open-addressed table of 64-bit hashes. Fixed because this runs on a
// streaming path: no allocation, no lock, one interlocked compare-exchange per
// new value and none at all for a value already present. A full table stops
// counting and raises `overflow` instead of silently under-reporting, so an
// unbounded spread of values is visible as such rather than reading as a
// plausible number just under the cap.

const int PXP_SET_SLOTS = 512;

// Probe callers on any load thread atomically insert hashes and update each
// total; main heartbeat reads totals independently. Each CAS/counter update
// publishes itself, so mixed diagnostic totals are tolerated. Reset only at
// installation before the detour is live (or isolated host tests).
struct PhysXPoolSet
{
	volatile __int64 slot[PXP_SET_SLOTS];   // 0 = empty; a hash is forced non-zero
	volatile long    distinct;
	volatile long    overflow;              // insertions refused because the table was full
	volatile long    samples;               // values offered, distinct or not
};

enum PhysXPoolInsert
{
	PXP_INSERT_NEW = 0,
	PXP_INSERT_PRESENT,
	PXP_INSERT_FULL
};

void PhysXPoolSetReset(PhysXPoolSet* set);
int  PhysXPoolSetInsert(PhysXPoolSet* set, unsigned __int64 key);

// FNV-1a. `seed` chains several fields into one key without an intermediate
// buffer; pass PXP_HASH_SEED to start.
const unsigned __int64 PXP_HASH_SEED = 0xCBF29CE484222325ULL;
unsigned __int64 PhysXPoolHashBytes(const void* bytes, size_t count, unsigned __int64 seed);
unsigned __int64 PhysXPoolHashU32(unsigned int value, unsigned __int64 seed);

// Three floats hashed by their bit patterns, because the name the engine
// interns is built from their decimal spelling: two scales that differ in any
// bit are two pooled strings.
unsigned __int64 PhysXPoolHashScale(float x, float y, float z, unsigned __int64 seed);


// ---------------------------------------------------------------------------
// Readout
// ---------------------------------------------------------------------------
//
// Zero is a meaningful reading here -- "no navmesh entrant ever ran" is a
// result -- so nothing that was never sampled may print as 0. It prints "?".

// "<n>", "<n>+" when the table overflowed, or "?" when nothing was offered.
size_t PhysXPoolFormatSet(char* out, size_t cap, const PhysXPoolSet* set);

// "<value>" when `sampled`, "?" otherwise.
size_t PhysXPoolFormatCount(char* out, size_t cap, long value, bool sampled);

#endif // KENSHI_ZONE_OPT_DIAG_PHYSX_POOL_SETS_H
