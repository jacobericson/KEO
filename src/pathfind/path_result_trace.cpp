// path_result_trace.cpp - The movement trace's path-thread copy: the request's own result array (64-byte
// EdgePathNode records, still in the shifted Havok frame when the hooks run) into one slot of a ring,
// one writer (the contentStream path thread) and one reader (the main thread). The copy takes no lock,
// allocates nothing and logs nothing. The release build compiles the whole body out.
#include "pathfind/path_result_trace.h"
#ifdef KEO_DEBUG
#include "base/core.h"
#include "base/clock.h"
#include "game/game.h"
#include <string.h>

static const size_t      EDGE_PATH_NODE_BYTES = 64;   // EdgePathNode's size (the layout's assertion)
static TraceResultRing   s_ring;
static volatile LONG     s_armed = 0;

void PathResultTraceArm()
{
	InterlockedExchange(&s_armed, 1);
}

void PathResultTraceCopy(void* navMesh, const void* resultBuf, const void* startPos, int ok, int player)
{
	if (!s_armed || !ok || !player || !navMesh || !resultBuf || !startPos)
		return;
	const char* nodes = *(const char* const*)(KLIB_MEMBER(5, resultBuf, ResultPathArray_m_data, 0));
	int count = *(const int*)(KLIB_MEMBER(5, resultBuf, ResultPathArray_m_size, 8));
	const float* shift = *(const float* const*)(KLIB_MEMBER(4, navMesh, NavMesh_worldShift, 0x1D8));
	if (!nodes || count <= 0 || !shift)
		return;
	TraceResult* r = TraceResultBegin(&s_ring);
	r->t = ElapsedSec();
	r->count = count;
	r->copied = count < TRACE_RESULT_NODES ? count : TRACE_RESULT_NODES;
	r->cut = count > TRACE_RESULT_NODES ? 1 : 0;
	memcpy(r->shift, shift, sizeof(r->shift));
	memcpy(r->start, startPos, sizeof(r->start));
	for (int i = 0; i < r->copied; ++i)
	{
		const char* node = nodes + (size_t)i * EDGE_PATH_NODE_BYTES;
		const float* left  = (const float*)(KLIB_MEMBER(5, node, EdgePathNode_mLeft, 0));
		const float* right = (const float*)(KLIB_MEMBER(5, node, EdgePathNode_mRight, 0x10));
		r->face[i] = *(const unsigned*)(KLIB_MEMBER(5, node, EdgePathNode_face, 0x20));
		for (int k = 0; k < 3; ++k)
			r->mid[i][k] = (left[k] + right[k]) * 0.5f;
	}
	TraceResultEnd(&s_ring);
}

bool PathResultTraceTake(long* taken, TraceResult* out, long* overruns, long* torn)
{
	return TraceResultTake(&s_ring, taken, out, overruns, torn);
}

#endif // KEO_DEBUG
