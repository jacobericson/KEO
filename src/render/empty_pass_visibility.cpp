#include "render/empty_pass_visibility.h"
#include "render/compositor_layout.h"
#include "render/empty_pass_policy.h"
#include "render/pass_visibility.h"

// A pass draws only what MovableObject::cullFrustum keeps from its queues:
// the objects whose flags pass the mask and whose world AABB meets the pass
// camera's frustum. Both are read here from the arrays that cull reads. They
// are this frame's: Root::renderOneFrame runs updateSceneGraph (transforms
// and bounds) just before the compositor update, and nothing between here and
// the passes' culls rewrites the bounds, flags or camera planes read here.
// The planes come from the camera's own getFrustumPlanes, the call
// _cullPhase01 makes before culling; a camera whose aspect the pass would
// still change is left undecided.

// SceneManager's cull list holds the dynamic and static entity managers.
static const size_t MAX_CULL_MANAGERS = 16;
// Distinct cameras among one node's scene passes.
static const size_t MAX_PASS_CAMERAS = 4;
// Objects tested per node and frame; past it the node stays on, so the test
// never costs more than a fixed, small share of the pass it would save.
static const size_t MAX_SCAN_OBJECTS = 2048;

// _getPasses returns a const vector<CompositorPass*>&: first, last, end.
typedef void* const* (*GetPasses_t)(const void* node);
typedef float (*CameraAspect_t)(void* camera);
typedef const void* (*CameraPlanes_t)(void* camera);

static GetPasses_t s_getPasses     = NULL;
static const void* s_passSceneVtbl = NULL;
static bool        s_ready         = false;

bool InstallPassVisibility(HMODULE ogre)
{
	s_ready = false;
	if (!VerifyPassVisibilityLayout(ogre))
		return false;
	s_getPasses = (GetPasses_t)GetProcAddress(ogre, SYM_NODE_GET_PASSES);
	s_passSceneVtbl = (const void*)GetProcAddress(ogre, SYM_PASS_SCENE_VTABLE);
	s_ready = s_getPasses && s_passSceneVtbl;
	return s_ready;
}

bool PassVisibilityAvailable()
{
	return s_ready;
}

static bool CullManagers(void* scene, void* const** list, void* const** end)
{
	*list = *(void* const* const*)((char*)scene + SM_CULL_LIST);
	*end = *(void* const* const*)((char*)scene + SM_CULL_LIST + 8);
	return *list && *end >= *list && (size_t)(*end - *list) <= MAX_CULL_MANAGERS;
}

static void QueueSlots(void* manager, const char** slots, const char** slotsEnd)
{
	*slots = *(const char* const*)((char*)manager + OMM_QUEUES);
	*slotsEnd = *(const char* const*)((char*)manager + OMM_QUEUES + 8);
}

static bool QueuesOccupied(void* const* list, void* const* end, size_t first, size_t last)
{
	for (void* const* m = list; m != end; ++m)
	{
		if (!*m)
			continue;
		const char* slots;
		const char* slotsEnd;
		QueueSlots(*m, &slots, &slotsEnd);
		for (size_t rq = first; rq < last; ++rq)
		{
			if (LiveObjectsInQueue(slots, slotsEnd, rq))
				return true;
		}
	}
	return false;
}

// The planes of each distinct camera the node's scene passes cull with,
// refreshed now. False when a pass has no camera or viewport, or its camera's
// aspect would change when the pass runs, or the node has no scene pass.
static bool PassCameraPlanes(void* node, const FrustumPlane** planes, size_t* count)
{
	void* cameras[MAX_PASS_CAMERAS];
	*count = 0;
	void* const* vec = s_getPasses(node);
	char* const* pass = (char* const*)vec[0];
	char* const* last = (char* const*)vec[1];
	if (!pass || last < pass)
		return false;
	for (; pass != last; ++pass)
	{
		if (!*pass || *(const void* const*)*pass != s_passSceneVtbl)
			continue;
		void* camera = *(void* const*)(*pass + PASS_CAMERA);
		const char* viewport = *(const char* const*)(*pass + PASS_VIEWPORT);
		if (!camera || !viewport)
			return false;
		void* const* vtbl = *(void* const* const*)camera;
		if (*(const bool*)((const char*)camera + CAM_AUTO_ASPECT))
		{
			int width = *(const int*)(viewport + VP_ACT_WIDTH);
			int height = *(const int*)(viewport + VP_ACT_HEIGHT);
			if (width <= 0 || height <= 0)
				return false;
			float aspect = (float)width / (float)height;
			if (!(((CameraAspect_t)vtbl[CAM_GET_ASPECT_SLOT])(camera) == aspect))
				return false;
		}
		size_t c = 0;
		while (c < *count && cameras[c] != camera)
			++c;
		if (c < *count)
			continue;
		if (*count == MAX_PASS_CAMERAS)
			return false;
		((CameraPlanes_t)vtbl[CAM_GET_PLANES_SLOT])(camera);
		cameras[*count] = camera;
		planes[*count] = (const FrustumPlane*)((const char*)camera + CAM_FRUSTUM_PLANES);
		++*count;
	}
	return *count > 0;
}

PassContent NodePassContent(void* node, void* scene, size_t firstRq, size_t endRq, bool resizePending,
                            size_t* scanned)
{
	void* const* list;
	void* const* listEnd;
	if (!CullManagers(scene, &list, &listEnd))
		return PASS_UNSURE;
	if (!QueuesOccupied(list, listEnd, firstRq, endRq))
		return PASS_EMPTY;
	if (!s_ready)
		return PASS_OCCUPIED;
	if (resizePending)
		return PASS_UNSURE;
	const FrustumPlane* planes[MAX_PASS_CAMERAS];
	size_t cameraCount = 0;
	if (!PassCameraPlanes(node, planes, &cameraCount))
		return PASS_UNSURE;
	size_t budget = MAX_SCAN_OBJECTS;
	PassContent result = PASS_HIDDEN;
	for (void* const* m = list; m != listEnd && result == PASS_HIDDEN; ++m)
	{
		if (!*m)
			continue;
		const char* slots;
		const char* slotsEnd;
		QueueSlots(*m, &slots, &slotsEnd);
		for (size_t rq = firstRq; rq < endRq && result == PASS_HIDDEN; ++rq)
		{
			if (!LiveObjectsInQueue(slots, slotsEnd, rq))
				continue;
			QueueObjects q;
			if (!QueueSlotObjects(slots + rq * QUEUE_SLOT_SIZE, &q))
			{
				result = PASS_UNSURE;
				break;
			}
			ScanResult r = ScanQueueObjects(q, planes, cameraCount, &budget);
			if (r == SCAN_MAY_DRAW)
				result = PASS_MAY_DRAW;
			else if (r == SCAN_OVER_BUDGET)
				result = PASS_UNSURE;
		}
	}
	*scanned += MAX_SCAN_OBJECTS - budget;
	return result;
}
