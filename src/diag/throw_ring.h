// throw_ring.h — where a first-chance C++ throw is recorded until something
// can afford to write it down.
//
// The throw's type is knowable only first-chance, and that runs on whatever
// thread threw, wherever it threw: inside the section manager's change mutex,
// inside a navmesh worker's job lock, in the middle of a pass a frame is
// waiting on. A file write there costs the lock's whole wait to everything
// else, and the game throws routinely on exactly those paths.
//
// So the capture puts a finished line into a fixed ring and returns. No
// allocation, no CRT, no lock, no syscall. The ring is drained through a sink
// the caller supplies, which is the only way text leaves this file — so the
// capture path cannot perform I/O however the caller is written. The drains
// are the death paths, where the process is ending and the cost is free.
//
// An overrun overwrites the oldest entry rather than dropping the newest: the
// throw nearest the death is the one worth keeping. What was lost is counted.

#ifndef KEO_DIAG_THROW_RING_H
#define KEO_DIAG_THROW_RING_H

#include <stddef.h>

enum
{
	THROW_RING_SLOTS = 16,
	THROW_RING_CHARS = 224
};

// One record's fields. `kind` and `type` may be NULL or empty; `phase` names
// the teardown state in the record's own words, so no token reserved for a
// real fault is spent on a caught throw.
struct ThrowRecordFields
{
	long             seq;
	unsigned long    code;
	unsigned __int64 addr;
	unsigned __int64 rva;
	unsigned long    tid;
	long             atSec;
	bool             afterStop;
	const char*      kind;
	const char*      type;
};

// Formats one record into `out`, NUL-terminated, truncating to fit. Returns
// the length written.
size_t ThrowRecordFormat(char* out, size_t cap, const ThrowRecordFields& f);

// Copies `text` into the next slot. Safe from a first-chance handler.
void ThrowRingPush(const char* text);

long ThrowRingPushed();       // records captured this process
long ThrowRingLost();         // captured, then overwritten before any drain
long ThrowRingPending();      // records a drain would deliver right now

typedef void (*ThrowRingSink)(void* ctx, const char* text, size_t len);

// Delivers the pending records to `sink`, oldest first, and marks them
// delivered. Returns how many were delivered.
long ThrowRingDrain(ThrowRingSink sink, void* ctx);

void ThrowRingResetForTest();

#endif // KEO_DIAG_THROW_RING_H
