// camera_focus.h — where the camera is looking, for zone-prediction purposes.
//
// A zoomed-out, rearward-pitched camera sits over the zone the squad is
// leaving even while the view and the squad head into the next one, because
// the mod's preload prediction used the camera's own (eye) position. This
// picks a better point: the game's own orbit/follow anchor (CameraClass's
// `center` node, read through PlayerInterface::getCameraCenter), clamped to
// stay near the squad so a detached or free-roaming camera never preloads a
// distant view.
//
// This header is host-testable: no game.h/core.h dependency, no KenshiLib
// types. tools/tests/camera_focus_units.cpp links this .cpp alone.

#ifndef KEO_CAMERA_FOCUS_H
#define KEO_CAMERA_FOCUS_H

// Why the candidate point was (or wasn't) used. CAMFOCUS_HORIZON is unused by
// the orbit-anchor method below (it never fails that way); it exists so a
// future ray-cast candidate can report it through the same counters.
enum CameraFocusReason
{
	CAMFOCUS_USED = 0,
	CAMFOCUS_CLAMPED,
	CAMFOCUS_FAR,
	CAMFOCUS_HORIZON,
	CAMFOCUS_INVALID,
	CAMFOCUS_DETACHED
};

struct CameraFocusInput
{
	float eyeX, eyeZ;                    // fallback: the camera's own position (today's behaviour)
	bool  haveFocus;                     // orbit-anchor point was readable this frame
	float focusX, focusZ;                // orbit-anchor point (only meaningful if haveFocus)
	bool  detached;                      // free camera, not following anyone, or inside a building
	bool  haveAnchor;                    // a squad member position is available
	float anchorX, anchorZ;              // nearest squad member (only meaningful if haveAnchor)
};

struct CameraFocusResult
{
	float x, z;
	CameraFocusReason reason;
};

// Pure decision: NaN/Inf/missing input, detachment, and the two-tier distance
// cap (soft clamp at maxDist, hard reject at hardDist) against the squad
// anchor. Always returns a finite point -- eyeX/eyeZ, completely unmodified,
// whenever detachment, invalid input or the hard cap applies -- and the
// continuous focus/clamped point otherwise, never quantized to a zone cell.
CameraFocusResult ComputeCameraFocusPoint(const CameraFocusInput& in, float maxDist, float hardDist);

// True for CAMFOCUS_USED/CAMFOCUS_CLAMPED (the continuous orbit-anchor
// point); false for every fallback reason (the raw eye). camera_zone_hook.cpp resets
// the prediction-axis hysteresis whenever this changes between frames, in
// either direction, so a held prediction from one point source never
// carries over to the other.
bool CameraFocusReasonIsFocusSource(CameraFocusReason reason);

// Whether `center` counts as stale this frame (zone_helpers.h's IsCameraDetached
// reads the four game-side conditions and combines them with this). Kept as
// a pure OR so every combination is host-testable without game.h.
bool CameraFocusIsStale(bool freeCameraMode, bool hasCenterBuilding,
                         bool followingNullItem, bool loadingOrPaused);

// True when a point's zone-grid coordinates -- (worldX - originX) / stepX,
// same for Z, not yet clamped or rounded -- land inside the grid, with one
// cell of margin at each edge. A point this far outside is a coordinate-space
// mismatch or a huge stray distance, not merely an edge zone: the caller
// rejects it as CAMFOCUS_INVALID rather than silently clamping it onto the
// border cell the way WorldToZoneGrid's own rounding would.
bool CameraFocusPointInGrid(float relX, float relZ, int gridMax);

struct CameraFocusAxisResult
{
	int  offset;  // -1, 0 or +1: the predicted neighbour along this axis
	bool held;    // true when hysteresis alone kept `offset` this frame
};

// Hysteresis for one prediction axis (X or Z, decided independently, exactly
// as camera_zone_hook.cpp's threshold check already treats them). `prevOffset` is the
// offset currently held for this axis; `d` is the signed distance from the
// current zone's centre along it. Once an offset is held it survives until
// `d`'s sign no longer agrees with it or |d| drops below threshold-margin;
// committing to a *new* offset (including re-entering the same one after a
// release) needs |d| to exceed threshold+margin. `held` reports a frame
// where |d| already fell back under threshold but hysteresis kept the
// previous offset anyway -- a border a plain threshold check would have
// flip-flopped on. margin <= 0 reproduces a plain single-threshold check.
CameraFocusAxisResult CameraFocusAxisHysteresis(int prevOffset, float d, float threshold, float margin);

#endif // KEO_CAMERA_FOCUS_H
