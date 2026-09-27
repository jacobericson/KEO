#pragma once
#include <stddef.h>

// Ogre::ObjectMemoryManager keeps one slot manager per render queue, an
// array [slots, slotsEnd) of QUEUE_SLOT_SIZE-byte entries indexed by queue
// ID. A queue's live object count is its used count less its free-slot list,
// as ObjectMemoryManager::getNumRenderQueues counts it; a queue past the
// array's end has never held an object.
static const size_t QUEUE_SLOT_SIZE  = 160;
static const size_t QUEUE_USED       = 0x40;   // size_t: slots handed out
static const size_t QUEUE_FREE_BEGIN = 0x60;   // size_t*: freed slots (a vector)
static const size_t QUEUE_FREE_END   = 0x68;

inline size_t LiveObjectsInQueue(const char* slots, const char* slotsEnd, size_t rq)
{
	if (!slots || slotsEnd <= slots || rq >= (size_t)(slotsEnd - slots) / QUEUE_SLOT_SIZE)
		return 0;
	const char* q = slots + rq * QUEUE_SLOT_SIZE;
	size_t used = *(const size_t*)(q + QUEUE_USED);
	const char* freeBegin = *(const char* const*)(q + QUEUE_FREE_BEGIN);
	const char* freeEnd = *(const char* const*)(q + QUEUE_FREE_END);
	size_t freed = freeEnd > freeBegin ? (size_t)(freeEnd - freeBegin) / sizeof(size_t) : 0;
	return used > freed ? used - freed : 0;
}

// Whether the interior-mask node should run this frame. Its texture is read
// by later passes and keeps its contents while the node is off, so it goes
// off only once it has run a frame with nothing to draw (its clear then left
// exactly what an empty pass leaves), and it runs in any frame that recreates
// the texture (a target resize). ranEmpty carries that from frame to frame.
inline bool MaskNodeWanted(bool drawing, bool resizePending, bool enabledNow, bool* ranEmpty)
{
	if (drawing)
	{
		*ranEmpty = false;
		return true;
	}
	if (resizePending || (enabledNow && !*ranEmpty))
	{
		*ranEmpty = true;
		return true;
	}
	return false;
}
