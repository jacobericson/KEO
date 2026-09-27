#include "render/shadow_receiver_depth.h"
#include "render/shadow_reach_math.h"
#include <windows.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <iomanip>
#include <sstream>

// Folds the nearest depth of every object the main camera's cull keeps. It
// is not a trustworthy receiver bound: the terrain is a single MovableObject
// whose patches are added after the cull, so one box stands for all of it,
// and the fold cannot tell G-buffer receivers from sky, water or transparent
// queues. So the candidate is only logged, in two forms: over every kept
// object, and over the finite boxes no larger than BIG_EXTENT, which leaves
// the terrain out and shows how far such a bound would be from the truth.
//
// Minimums fold across the main camera's culls from one shadow render to the
// next, which covers this frame's G-buffer cull but also the passes after
// the previous shadow render; for a dry run that is enough.

// Ogre::Camera's derived orientation (w, x, y, z) and position, as its own
// cull reads them.
static const size_t CAM_DERIVED_ORIENTATION = 0x554;
static const size_t CAM_DERIVED_POSITION    = 0x564;

static const float BIG_EXTENT = 500.0f;
static const int   MAX_CASCADES = 4;

// FloatOrderKey(+infinity): nothing folded yet.
static const LONG EMPTY_KEY = 0x7F800000;

static const void* volatile s_camera = NULL;   // the main camera being folded
static volatile LONG s_minAll   = EMPTY_KEY;
static volatile LONG s_minSmall = EMPTY_KEY;
static volatile LONG s_infCount = 0;
static volatile LONG s_bigCount = 0;

// Main thread only: the window's frames and the last frame's snapshot.
struct DepthWindow
{
	LONG  frames;                 // shadow renders with a candidate
	LONG  wouldAll[MAX_CASCADES];
	LONG  wouldSmall[MAX_CASCADES];
	LONG  overFocusAll, overFocusSmall;
	bool  haveLast;
	float lastAll, lastSmall, lastFocus;
	int   lastCascades;
	float lastSplits[MAX_CASCADES + 1];
};
static DepthWindow s_window;

static void FoldMin(volatile LONG* target, float v)
{
	LONG key = (LONG)FloatOrderKey(v);
	LONG cur = *target;
	while (key < cur)
	{
		LONG seen = InterlockedCompareExchange(target, key, cur);
		if (seen == cur)
			break;
		cur = seen;
	}
}

void ReceiverDepth_Reset()
{
	s_camera = NULL;
	InterlockedExchange(&s_minAll, EMPTY_KEY);
	InterlockedExchange(&s_minSmall, EMPTY_KEY);
	InterlockedExchange(&s_infCount, 0);
	InterlockedExchange(&s_bigCount, 0);
	memset(&s_window, 0, sizeof(s_window));
}

bool ReceiverDepth_Watches(const void* frustum)
{
	return frustum && frustum == s_camera;
}

void ReceiverDepth_Fold(const void* camera, void* const* data, size_t begin, size_t end, WorldAabbFn aabb)
{
	if (!data || end <= begin)
		return;
	float q[4], pos[3], fwd[3];
	memcpy(q, (const char*)camera + CAM_DERIVED_ORIENTATION, sizeof(q));
	memcpy(pos, (const char*)camera + CAM_DERIVED_POSITION, sizeof(pos));
	QuaternionForward(q, fwd);
	if (!_finite(fwd[0]) || !_finite(fwd[1]) || !_finite(fwd[2]) ||
	    !_finite(pos[0]) || !_finite(pos[1]) || !_finite(pos[2]))
		return;

	float minAll = FLT_MAX, minSmall = FLT_MAX;
	LONG inf = 0, big = 0;
	for (size_t k = begin; k < end; ++k)
	{
		const void* obj = data[k];
		if (!obj)
			continue;
		float box[6];
		aabb(obj, box);
		if (!AabbIsFinite(box, box + 3))
		{
			++inf;
			if (minAll > 0.0f)
				minAll = 0.0f;
			continue;
		}
		float d = AabbMinDepth(pos, fwd, box, box + 3);
		if (d < minAll)
			minAll = d;
		float h = box[3] > box[4] ? box[3] : box[4];
		if (box[5] > h) h = box[5];
		if (2.0f * h > BIG_EXTENT)
			++big;
		else if (d < minSmall)
			minSmall = d;
	}
	if (minAll < FLT_MAX)
		FoldMin(&s_minAll, minAll);
	if (minSmall < FLT_MAX)
		FoldMin(&s_minSmall, minSmall);
	if (inf)
		InterlockedExchangeAdd(&s_infCount, inf);
	if (big)
		InterlockedExchangeAdd(&s_bigCount, big);
}

void ReceiverDepth_OnShadowStart(const float* splits, int cascades, const void* mainCamera, float focusDistance)
{
	float all = FloatFromOrderKey((int)InterlockedExchange(&s_minAll, EMPTY_KEY));
	float small = FloatFromOrderKey((int)InterlockedExchange(&s_minSmall, EMPTY_KEY));
	s_camera = mainCamera;
	if (!splits || cascades < 1 || cascades > MAX_CASCADES || !_finite(all))
		return;

	unsigned maskAll = CascadeSkipMask(splits, cascades, all, CASCADE_DEPTH_MARGIN);
	unsigned maskSmall = CascadeSkipMask(splits, cascades, small, CASCADE_DEPTH_MARGIN);
	++s_window.frames;
	for (int i = 0; i < cascades; ++i)
	{
		if (maskAll & (1u << i)) ++s_window.wouldAll[i];
		if (maskSmall & (1u << i)) ++s_window.wouldSmall[i];
	}
	if (focusDistance > 0.0f)
	{
		if (all > focusDistance) ++s_window.overFocusAll;
		if (_finite(small) && small > focusDistance) ++s_window.overFocusSmall;
	}
	s_window.haveLast = true;
	s_window.lastAll = all;
	s_window.lastSmall = small;
	s_window.lastFocus = focusDistance;
	s_window.lastCascades = cascades;
	for (int i = 0; i <= cascades; ++i)
		s_window.lastSplits[i] = splits[i];
}

static void PutDepth(std::ostringstream& ss, float v)
{
	if (_finite(v))
		ss << v;
	else
		ss << "-";
}

void ReceiverDepth_ClearWindow()
{
	InterlockedExchange(&s_infCount, 0);
	InterlockedExchange(&s_bigCount, 0);
	memset(&s_window, 0, sizeof(s_window));
}

std::string ReceiverDepthToken(double windowSec)
{
	double perSec = windowSec > 0.0 ? 1.0 / windowSec : 0.0;
	LONG inf = InterlockedExchange(&s_infCount, 0);
	LONG big = InterlockedExchange(&s_bigCount, 0);
	DepthWindow w = s_window;
	memset(&s_window, 0, sizeof(s_window));

	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(0) << " depth=";
	if (!w.haveLast)
	{
		ss << "-";
		return ss.str();
	}
	ss << "all";
	PutDepth(ss, w.lastAll);
	ss << "/small";
	PutDepth(ss, w.lastSmall);
	ss << " focus=";
	if (w.lastFocus > 0.0f)
		ss << w.lastFocus;
	else
		ss << "-";
	ss << " split=";
	for (int i = 1; i <= w.lastCascades; ++i)
		ss << (i > 1 ? "/" : "") << w.lastSplits[i];
	ss << " inf=" << (double)inf * perSec << " big=" << (double)big * perSec
	   << " frames=" << w.frames << " dry=all:";
	for (int i = 0; i < w.lastCascades; ++i)
		ss << (i ? "/" : "") << w.wouldAll[i];
	ss << ",small:";
	for (int i = 0; i < w.lastCascades; ++i)
		ss << (i ? "/" : "") << w.wouldSmall[i];
	ss << " overFocus=" << w.overFocusAll << "/" << w.overFocusSmall;
	return ss.str();
}
