#pragma once
#include <stddef.h>

// Whether a render_scene pass could draw anything, answered from the same
// data Ogre::MovableObject::cullFrustum reads: the render queue's object
// arrays and the pass camera's frustum planes. Pure: no game or Ogre calls.

// Ogre::Plane as the camera stores its six frustum planes.
struct FrustumPlane { float nx, ny, nz, d; };
static const size_t FRUSTUM_PLANE_COUNT = 6;

// cullFrustum keeps an object only if its visibility flags share a bit with
// the pass mask, which Ogre first cuts to VIS_MASK_BITS, and carry
// LAYER_VISIBILITY (cleared by MovableObject::setVisible(false)).
static const unsigned int VIS_MASK_BITS    = 0x1FFFFFFF;
static const unsigned int LAYER_VISIBILITY = 0x40000000;

// A world AABB block holds four objects as six arrays of four floats:
// centre x, y, z, then half size x, y, z.
static const size_t OBJECTS_PER_BLOCK = 4;
static const size_t AABB_BLOCK_FLOATS = 24;

// Ogre's own margin is zero; this one only absorbs float rounding between
// the plane values read here and the ones the cull reads.
static const float PLANE_MARGIN_ABS = 1.0f;
static const float PLANE_MARGIN_REL = 1e-5f;

// True unless cullFrustum is certain to reject the object for every pass
// mask. It repeats cullFrustum's plane test with the same operations in the
// same order, so a NaN or a null box (half size -inf) is rejected exactly as
// Ogre rejects it and an infinite box (half size +inf) is kept. The pass mask,
// shadow-caster bit and rendering distance are left out, so the answer is
// never stricter than Ogre's. With `loosen`, each plane moves out by
// PLANE_MARGIN_ABS + PLANE_MARGIN_REL * |d|.
bool ObjectMayDraw(unsigned int visFlags, const float centre[3], const float half[3],
                   const FrustumPlane* planes, bool loosen);

// One render queue's object arrays, as ObjectMemoryManager keeps them per
// queue slot: `count` slots handed out, freed ones included (cull walks those
// too, rounded up to whole blocks).
struct QueueObjects
{
	const float*        worldAabb;   // AABB_BLOCK_FLOATS per block
	const unsigned int* visFlags;    // one per object
	size_t              count;
};

// ObjectMemoryManager queue slot fields beyond those in empty_pass_policy.h:
// a vector of pool pointers (the ObjectData arrays in declaration order) and
// the pools the cull reads.
static const size_t QUEUE_POOLS_BEGIN = 0x08;
static const size_t QUEUE_POOLS_END   = 0x10;
static const size_t POOL_WORLD_AABB   = 3;
static const size_t POOL_VIS_FLAGS    = 7;

// Reads a queue slot. False when the slot holds objects but its pool vector
// is too short or a needed pool is missing.
bool QueueSlotObjects(const char* slot, QueueObjects* out);

enum ScanResult
{
	SCAN_HIDDEN,       // no object in the queue can be drawn
	SCAN_MAY_DRAW,     // some object might be drawn by one of the cameras
	SCAN_OVER_BUDGET   // gave up: more objects than *budget allowed
};

// Tests the queue's objects against each camera's planes. *budget counts the
// objects still allowed and is lowered by the objects tested.
ScanResult ScanQueueObjects(const QueueObjects& q, const FrustumPlane* const* cameras, size_t cameraCount,
                            size_t* budget);
