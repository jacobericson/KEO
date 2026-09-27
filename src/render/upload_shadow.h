#pragma once
#include <cstddef>

// Copies of each constant buffer's last upload, keyed by buffer pointer, for
// one thread. No Windows or game calls, so the host tests link it directly.

// One shader variable of a copy plan: where its bytes go in the buffer and
// the constant definition that says where they come from.
struct UploadPlanStep
{
	size_t      dst;
	size_t      size;
	const void* def;       // GpuConstantDefinition, alive while the plan's map is
	int         type;      // def's type when the step was made
	bool        isFloat;
};

struct UploadShadow
{
	const void*    buffer;
	const void*    owner;   // the program's buffer node the copy was taken for
	unsigned char* bytes;
	size_t         size;    // of bytes
	bool           valid;   // bytes hold what the buffer holds now
	unsigned       gen;     // the table's copy generation the copy belongs to
	unsigned       planGen; // the table's plan generation the plan belongs to

	// The copy plan for owner's variables, resolved against planMap. The key
	// (planMap, planVars, planVarsEnd, planMapGen) is also kept for a refused
	// plan, so the same key is not tried again.
	UploadPlanStep* plan;
	int             planCount;
	int             planCap;
	bool            planValid;
	bool            planRefused;
	const void*     planMap;
	const void*     planVars;
	const void*     planVarsEnd;
	long            planMapGen;
};

// No constructor: a static instance starts zeroed, i.e. unconfigured and
// empty. The slot array is allocated at the first Acquire, so a table that
// is never used costs nothing.
struct UploadShadowTable
{
	UploadShadow* slots;
	int           capacity;   // power of two
	int           count;
	size_t        byteCap;    // running total of copy and plan bytes never exceeds this
	size_t        bytes;
	unsigned      gen;
	unsigned      planGen;

	// Frees everything first.
	void Configure(int slotCount, size_t maxBytes);

	// The copy for buffer. The copy restarts (valid false) when it is new,
	// when owner or size changed, or when it predates the last invalidation;
	// its plan (planValid and planRefused false) on the same events except
	// InvalidateCopies. NULL when the table is full, the byte cap would be
	// exceeded or an allocation fails; the table is then unchanged.
	UploadShadow* Acquire(const void* buffer, const void* owner, size_t size);

	// Room for steps plan steps in s, emptying its plan (planValid and
	// planRefused false, planCount 0). A new plan may cover bytes the copy
	// never took, so the copy restarts too (valid false). False, with the
	// plan empty, when the byte cap would be exceeded or the allocation fails.
	bool ReservePlan(UploadShadow* s, int steps);

	// Every copy restarts at its next Acquire; plans are kept.
	void InvalidateCopies();
	// Every copy and every plan restarts at its next Acquire.
	void InvalidateAll();

	// Frees every copy, every plan and the slot array; the configuration stays.
	void Clear();

	// Past half full, frees every copy and plan and empties the slots, so
	// buffers that no longer exist give their slots back; the slot array
	// stays. Called by both invalidations.
	void ReclaimIfCrowded();
};

// Compares src with the copy at offset and stores it there. False, with the
// copy unchanged, when [offset, offset + n) lies outside the copy; *differs
// is set when any byte changed.
bool UploadShadowUpdate(UploadShadow* s, size_t offset, const void* src, size_t n, bool* differs);
