#include "render/pass_visibility.h"
#include "render/empty_pass_policy.h"
#include <math.h>
#include <string.h>

// cullFrustum builds each plane's sign vector as (n & sign bit) | 1.0, so a
// -0 component counts as negative.
static float SignOf(float v)
{
	unsigned int bits;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x80000000u) ? -1.0f : 1.0f;
}

static bool IsPlusInfinity(float v)
{
	unsigned int bits;
	memcpy(&bits, &v, sizeof(bits));
	return bits == 0x7F800000u;
}

bool ObjectMayDraw(unsigned int visFlags, const float centre[3], const float half[3],
                   const FrustumPlane* planes, bool loosen)
{
	if ((visFlags & VIS_MASK_BITS) == 0 || (visFlags & LAYER_VISIBILITY) == 0)
		return false;
	if (IsPlusInfinity(half[0]) || IsPlusInfinity(half[1]) || IsPlusInfinity(half[2]))
		return true;
	for (size_t i = 0; i < FRUSTUM_PLANE_COUNT; ++i)
	{
		const FrustumPlane& p = planes[i];
		// The farthest box corner along the normal, summed as cullFrustum
		// sums it: z + (x + y). A NaN fails the comparison, as in Ogre.
		float x = (centre[0] + half[0] * SignOf(p.nx)) * p.nx;
		float y = (centre[1] + half[1] * SignOf(p.ny)) * p.ny;
		float z = (centre[2] + half[2] * SignOf(p.nz)) * p.nz;
		float along = z + (x + y);
		float negD = -p.d;
		if (loosen)
			negD -= PLANE_MARGIN_ABS + PLANE_MARGIN_REL * fabsf(p.d);
		if (!(negD < along))
			return false;
	}
	return true;
}

bool QueueSlotObjects(const char* slot, QueueObjects* out)
{
	out->worldAabb = NULL;
	out->visFlags = NULL;
	out->count = *(const size_t*)(slot + QUEUE_USED);
	if (out->count == 0)
		return true;
	const char* const* pools = *(const char* const* const*)(slot + QUEUE_POOLS_BEGIN);
	const char* const* poolsEnd = *(const char* const* const*)(slot + QUEUE_POOLS_END);
	if (!pools || poolsEnd < pools || (size_t)(poolsEnd - pools) <= POOL_VIS_FLAGS)
		return false;
	out->worldAabb = (const float*)pools[POOL_WORLD_AABB];
	out->visFlags = (const unsigned int*)pools[POOL_VIS_FLAGS];
	return out->worldAabb && out->visFlags;
}

ScanResult ScanQueueObjects(const QueueObjects& q, const FrustumPlane* const* cameras, size_t cameraCount,
                            size_t* budget)
{
	// cullFrustum tests whole blocks, the unused lanes of the last one too.
	size_t lanes = (q.count + OBJECTS_PER_BLOCK - 1) / OBJECTS_PER_BLOCK * OBJECTS_PER_BLOCK;
	for (size_t k = 0; k < lanes; ++k)
	{
		if (*budget == 0)
			return SCAN_OVER_BUDGET;
		--*budget;
		unsigned int flags = q.visFlags[k];
		if ((flags & VIS_MASK_BITS) == 0 || (flags & LAYER_VISIBILITY) == 0)
			continue;
		const float* block = q.worldAabb + (k / OBJECTS_PER_BLOCK) * AABB_BLOCK_FLOATS;
		size_t lane = k % OBJECTS_PER_BLOCK;
		float centre[3] = { block[lane], block[4 + lane], block[8 + lane] };
		float half[3] = { block[12 + lane], block[16 + lane], block[20 + lane] };
		for (size_t c = 0; c < cameraCount; ++c)
		{
			if (ObjectMayDraw(flags, centre, half, cameras[c], true))
				return SCAN_MAY_DRAW;
		}
	}
	return SCAN_HIDDEN;
}
