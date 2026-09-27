#pragma once
#include <stddef.h>

// Plane math for the shadow caster reach test. No platform or engine types:
// vectors are float[3], world space, y up.

// A half-space: n.p + c >= 0 is inside. n is not necessarily unit length.
struct ReachPlane
{
	float n[3];
	float c;
};

// How far past a split, in view depth, the shader still picks that split's
// cascade: about split + near (the main camera's near plane) under D3D
// depth, the only render system shipped; 10 leaves slack.
static const float CASCADE_DEPTH_MARGIN = 10.0f;

// Six slice planes eliminate to at most 12 (3 facing the light times 3
// facing away, plus those 3).
static const int REACH_MAX_PLANES = 12;

// The main camera as a symmetric perspective frustum. fwd, up and right are
// the derived unit axes (fwd is the view direction).
struct ReachCamera
{
	float pos[3];
	float fwd[3];
	float up[3];
	float right[3];
	float tanHalfFovY;
	float aspect;
};

// The six inward planes of the view frustum between view depths nearDepth
// and farDepth (along fwd), unit normals, each pushed outward by margin.
// Order: near, far, top, bottom, right, left.
void BuildSlicePlanes(const ReachCamera& cam, float nearDepth, float farDepth,
                      float margin, ReachPlane out[6]);

// E = { p : p + s*L lies inside every input plane for some s >= 0 }, the
// points from which light travelling along L can reach the slice. Planes
// with n.L == 0 or n.L < 0 are kept as they are; each pair with n_j.L > 0
// and n_i.L < 0 adds (a_j n_i - a_i n_j).p + (a_j c_i - a_i c_j) >= 0.
// Returns the plane count, or -1 when cap is too small.
int ExtrudeTowardLight(const ReachPlane* in, int count, const float L[3],
                       ReachPlane* out, int cap);

// Moves every plane by d along dir (the region shifts by d*dir).
void ShiftPlanes(ReachPlane* planes, int count, const float dir[3], float d);

// True when the box (center, halfSize grown by grow on each axis) lies
// entirely on the outside of at least one plane.
bool AabbOutsideAnyPlane(const ReachPlane* planes, int count,
                         const float center[3], const float halfSize[3], float grow);

// False for any infinite or NaN component.
bool AabbIsFinite(const float center[3], const float halfSize[3]);

// The side of the game's cascade box before its per-axis ceil, for a slice
// from depth n to f: max(2 f sqrt(K), sqrt((f+n)^2 K + (f-n)^2)) with
// K = (aspect*t)^2 + 2t, t = tan(fovY/2).
float CascadeBoxSide(float n, float f, float tanHalfFovY, float aspect);

// Largest distance between two points of the frustum from the camera to
// depth f; bounds the box's extent on any axis.
float FrustumDiameter(float f, float tanHalfFovY, float aspect);

// Sun elevation above the horizon, and its azimuth from the camera heading
// (0 ahead, +90 to the camera's right, 180 behind), both in degrees. L is the
// light's travel direction, so the sun lies along -L.
void SunElevationAzimuth(const float L[3], const float fwd[3], const float right[3],
                         float* elevationDeg, float* azimuthDeg);

// The light's travel direction from a vector pointing toward the sun:
// L = -normalize(towardSun). False for a zero-length or non-finite vector.
bool LightTravelDirection(const float towardSun[3], float L[3]);

// The -z axis of a unit quaternion in Ogre's (w, x, y, z) order: a camera's
// view direction.
void QuaternionForward(const float wxyz[4], float fwd[3]);

// The smallest view depth, along unit fwd from pos, of any point of the box.
float AabbMinDepth(const float pos[3], const float fwd[3], const float center[3], const float halfSize[3]);

// Bit i is set when every receiver of cascade i lies before dmin: its last
// receiver depth, split[i+1] + margin, is less than dmin. splits holds
// cascades + 1 depths. 0 when dmin is not finite or cascades is outside 1..8.
unsigned CascadeSkipMask(const float* splits, int cascades, float dmin, float margin);

// Integer keys whose signed order is the order of the floats they encode
// (NaN excluded), so an interlocked integer min is a float min.
int   FloatOrderKey(float v);
float FloatFromOrderKey(int key);

// One caster against a cascade's reach planes.
enum ReachVerdict
{
	RV_KEEP,            // reaches the slice
	RV_KEEP_BIG,        // reaches it, and its box is large
	RV_KEEP_INFINITE,   // infinite or NaN box, kept unexamined
	RV_CUT              // cannot shadow any receiver of the slice
};
typedef ReachVerdict (*ReachJudge)(const void* obj, void* ctx);

struct ReachTally
{
	long in, reach, cut, inf, big;
};

// Judges data[begin, end). With compact, the kept entries (NULLs included,
// which are not judged or counted) move down in order and the new end is
// returned; without it nothing moves and end is returned.
size_t FilterReach(void** data, size_t begin, size_t end, bool compact,
                   ReachJudge judge, void* ctx, ReachTally* tally);
