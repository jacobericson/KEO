// camera_focus.cpp — pure geometry and decision logic. See camera_focus.h.
// No game.h/core.h dependency: host-testable in isolation.

#include "zone/preload/camera_focus.h"
#include <cmath>

static bool IsFiniteFloat(float v)
{
	// NaN never equals itself; a finite-magnitude check catches +/-Inf and the
	// occasional garbage read without pulling in platform-specific isinf.
	return (v == v) && v > -3.0e38f && v < 3.0e38f;
}

CameraFocusResult ComputeCameraFocusPoint(const CameraFocusInput& in, float maxDist, float hardDist)
{
	CameraFocusResult r;
	r.x = in.eyeX;
	r.z = in.eyeZ;

	if (!in.haveFocus || !IsFiniteFloat(in.focusX) || !IsFiniteFloat(in.focusZ))
	{
		r.reason = CAMFOCUS_INVALID;
		return r;
	}

	if (in.detached)
	{
		r.reason = CAMFOCUS_DETACHED;
		return r;
	}

	if (!in.haveAnchor || !IsFiniteFloat(in.anchorX) || !IsFiniteFloat(in.anchorZ))
	{
		r.reason = CAMFOCUS_INVALID;
		return r;
	}

	float dx = in.focusX - in.anchorX;
	float dz = in.focusZ - in.anchorZ;
	float dist = sqrtf(dx * dx + dz * dz);

	if (dist > hardDist)
	{
		r.reason = CAMFOCUS_FAR;
		return r;
	}

	if (dist > maxDist)
	{
		float scale = (dist > 0.0001f) ? (maxDist / dist) : 0.0f;
		r.x = in.anchorX + dx * scale;
		r.z = in.anchorZ + dz * scale;
		r.reason = CAMFOCUS_CLAMPED;
		return r;
	}

	r.x = in.focusX;
	r.z = in.focusZ;
	r.reason = CAMFOCUS_USED;
	return r;
}

bool CameraFocusReasonIsFocusSource(CameraFocusReason reason)
{
	return reason == CAMFOCUS_USED || reason == CAMFOCUS_CLAMPED;
}

bool CameraFocusIsStale(bool freeCameraMode, bool hasCenterBuilding,
                         bool followingNullItem, bool loadingOrPaused)
{
	return freeCameraMode || hasCenterBuilding || followingNullItem || loadingOrPaused;
}

bool CameraFocusPointInGrid(float relX, float relZ, int gridMax)
{
	float hi = (float)(gridMax + 1);
	return relX > -1.0f && relX < hi && relZ > -1.0f && relZ < hi;
}

CameraFocusAxisResult CameraFocusAxisHysteresis(int prevOffset, float d, float threshold, float margin)
{
	CameraFocusAxisResult r;
	r.held = false;

	float ad = (d < 0.0f) ? -d : d;

	if (prevOffset != 0)
	{
		bool sameDirection = (prevOffset > 0) ? (d > 0.0f) : (d < 0.0f);
		if (sameDirection && ad >= threshold - margin)
		{
			r.offset = prevOffset;
			r.held = ad < threshold;  // a plain threshold check would have released it
			return r;
		}
		// direction reversed, or fell below the lower band: released. Falls
		// through to the plain commit test below, same as no prior offset.
	}

	if (d > threshold + margin)       r.offset = 1;
	else if (d < -(threshold + margin)) r.offset = -1;
	else                                 r.offset = 0;
	return r;
}
