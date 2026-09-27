#include <cstdio>
#include "zone/preload/camera_focus.h"

#include "check.h"

static CameraFocusInput MakeInput()
{
	CameraFocusInput in;
	in.eyeX = 1000.0f; in.eyeZ = 2000.0f;
	in.haveFocus = true; in.focusX = 1100.0f; in.focusZ = 2100.0f;
	in.detached = false;
	in.haveAnchor = true; in.anchorX = 1050.0f; in.anchorZ = 2050.0f;
	return in;
}

int main()
{
	const float maxDist = 8192.0f;
	const float hardDist = maxDist * 3.0f;

	// 1. Used: focus close to the squad anchor.
	{
		CameraFocusInput in = MakeInput();
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_USED, "close focus is used");
		Check(r.x == in.focusX && r.z == in.focusZ, "used point equals focus point");
	}

	// 2. Clamped: focus well past maxDist from the anchor but inside hardDist,
	//    pulled back along the anchor->focus direction to exactly maxDist.
	{
		CameraFocusInput in = MakeInput();
		in.anchorX = 0.0f; in.anchorZ = 0.0f;
		in.focusX = 20000.0f; in.focusZ = 0.0f;  // 20000 > maxDist(8192), < hardDist(24576)
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_CLAMPED, "over-cap focus is clamped");
		float dist = r.x - in.anchorX;
		Check(dist > maxDist - 1.0f && dist < maxDist + 1.0f, "clamped point sits at maxDist");
	}

	// 3. Far: focus beyond hardDist -- reject outright, fall back to the eye.
	{
		CameraFocusInput in = MakeInput();
		in.anchorX = 0.0f; in.anchorZ = 0.0f;
		in.focusX = 100000.0f; in.focusZ = 0.0f;
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_FAR, "beyond hardDist is far");
		Check(r.x == in.eyeX && r.z == in.eyeZ, "far falls back to the eye position");
	}

	// 4. Detached: free camera / no follow / interior -- always the eye, cap ignored.
	{
		CameraFocusInput in = MakeInput();
		in.detached = true;
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_DETACHED, "detached camera reports detached");
		Check(r.x == in.eyeX && r.z == in.eyeZ, "detached falls back to the eye position");
	}

	// 5. Invalid: unreadable focus pointer chain.
	{
		CameraFocusInput in = MakeInput();
		in.haveFocus = false;
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_INVALID, "unreadable focus is invalid");
		Check(r.x == in.eyeX && r.z == in.eyeZ, "invalid falls back to the eye position");
	}

	// 5b. Invalid: NaN focus coordinate.
	{
		CameraFocusInput in = MakeInput();
		float zero = 0.0f;
		in.focusX = zero / zero;  // NaN, computed so the compiler can't fold it away
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_INVALID, "NaN focus is invalid");
	}

	// 5c. Invalid: no squad anchor to check the cap against.
	{
		CameraFocusInput in = MakeInput();
		in.haveAnchor = false;
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_INVALID, "missing anchor is invalid");
	}

	// 6. Grid-bounds plausibility check (used by camera_zone_hook.cpp before WorldToZoneGrid).
	{
		Check(CameraFocusPointInGrid(0.0f, 0.0f, 63), "origin cell is in grid");
		Check(CameraFocusPointInGrid(63.0f, 63.0f, 63), "far corner cell is in grid");
		Check(CameraFocusPointInGrid(-0.9f, 10.0f, 63), "just past the near edge is in grid");
		Check(!CameraFocusPointInGrid(-5.0f, 10.0f, 63), "well past the near edge is rejected");
		Check(!CameraFocusPointInGrid(10.0f, 200.0f, 63), "far past the map is rejected");
	}

	// 7. Axis hysteresis: no prior offset, distance under threshold+margin ->
	//    stays at 0 (the plain-threshold case too, at margin 0).
	{
		CameraFocusAxisResult r = CameraFocusAxisHysteresis(0, 2000.0f, 2500.0f, 250.0f);
		Check(r.offset == 0 && !r.held, "under threshold+margin: no prediction yet");
	}

	// 8. Axis hysteresis: a real crossing (past threshold+margin) commits.
	{
		CameraFocusAxisResult r = CameraFocusAxisHysteresis(0, 2900.0f, 2500.0f, 250.0f);
		Check(r.offset == 1 && !r.held, "past threshold+margin commits to +1");
	}

	// 9. Axis hysteresis: oscillation around the plain threshold, with an
	//    active offset, holds a single prediction instead of flip-flopping.
	//    d=2900 commits (>2750); d=2400 is below the plain 2500 threshold but
	//    still above 2500-250=2250, so hysteresis keeps holding +1.
	{
		CameraFocusAxisResult r1 = CameraFocusAxisHysteresis(0, 2900.0f, 2500.0f, 250.0f);
		Check(r1.offset == 1, "setup: commits to +1");
		CameraFocusAxisResult r2 = CameraFocusAxisHysteresis(r1.offset, 2400.0f, 2500.0f, 250.0f);
		Check(r2.offset == 1 && r2.held, "oscillation just under threshold holds, and is counted as held");
		CameraFocusAxisResult r3 = CameraFocusAxisHysteresis(r2.offset, 2900.0f, 2500.0f, 250.0f);
		Check(r3.offset == 1 && !r3.held, "back over the plain threshold: still held, no longer counted");
	}

	// 10b. Axis hysteresis: a real release (below threshold-margin) and a
	//      real reversal (crossing to the other neighbour) both switch.
	{
		CameraFocusAxisResult held = CameraFocusAxisHysteresis(0, 2900.0f, 2500.0f, 250.0f);
		CameraFocusAxisResult released = CameraFocusAxisHysteresis(held.offset, 2000.0f, 2500.0f, 250.0f);
		Check(released.offset == 0 && !released.held, "dropping below threshold-margin releases the hold");
		CameraFocusAxisResult reversed = CameraFocusAxisHysteresis(held.offset, -2900.0f, 2500.0f, 250.0f);
		Check(reversed.offset == -1, "a real crossing to the other side switches immediately");
	}

	// 10. Regression: the used/clamped point is continuous, never snapped to a
	//     zone-cell multiple. (camera_zone_hook.cpp's CameraFocusApply must feed this
	//     straight into the threshold math, not a quantized grid center.)
	{
		CameraFocusInput in = MakeInput();
		in.anchorX = 1000.0f; in.anchorZ = 2000.0f;
		in.focusX = 1123.4f; in.focusZ = 2087.6f;  // not a multiple of any plausible zone step
		CameraFocusResult r = ComputeCameraFocusPoint(in, maxDist, hardDist);
		Check(r.reason == CAMFOCUS_USED, "setup: still inside the cap");
		Check(r.x == 1123.4f && r.z == 2087.6f, "used point keeps its exact fractional position");
	}

	// 11. Stale-camera predicate: each of the four conditions alone is stale,
	//     and none of them together is not.
	{
		Check(CameraFocusIsStale(true, false, false, false), "free camera mode is stale");
		Check(CameraFocusIsStale(false, true, false, false), "inside a building is stale");
		Check(CameraFocusIsStale(false, false, true, false), "following NULL_ITEM is stale");
		Check(CameraFocusIsStale(false, false, false, true), "loading or paused is stale");
		Check(!CameraFocusIsStale(false, false, false, false), "none of the four is not stale");
	}

	// 12. Point-source classification: USED/CLAMPED are the continuous focus;
	//     every fallback reason is the raw eye. camera_zone_hook.cpp resets the
	//     prediction hysteresis whenever this flips between frames.
	{
		Check(CameraFocusReasonIsFocusSource(CAMFOCUS_USED), "USED is the focus source");
		Check(CameraFocusReasonIsFocusSource(CAMFOCUS_CLAMPED), "CLAMPED is the focus source");
		Check(!CameraFocusReasonIsFocusSource(CAMFOCUS_FAR), "FAR is the eye source");
		Check(!CameraFocusReasonIsFocusSource(CAMFOCUS_DETACHED), "DETACHED is the eye source");
		Check(!CameraFocusReasonIsFocusSource(CAMFOCUS_INVALID), "INVALID is the eye source");
		Check(!CameraFocusReasonIsFocusSource(CAMFOCUS_HORIZON), "HORIZON is the eye source");
	}

	return CheckExit("camera_focus_units");
}
