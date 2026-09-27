#include "render/shadow_reach_math.h"
#include <math.h>
#include <float.h>
#include <string.h>

static float Dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Plane through the camera position with normal a*u + b*v, normalized, then
// pushed outward by margin.
static void SidePlane(const ReachCamera& cam, float a, const float u[3], float b, const float v[3],
                      float margin, ReachPlane* p)
{
	float n[3] = { a * u[0] + b * v[0], a * u[1] + b * v[1], a * u[2] + b * v[2] };
	float len = sqrtf(Dot(n, n));
	float inv = len > 0.0f ? 1.0f / len : 0.0f;
	for (int k = 0; k < 3; ++k)
		p->n[k] = n[k] * inv;
	p->c = -Dot(p->n, cam.pos) + margin;
}

void BuildSlicePlanes(const ReachCamera& cam, float nearDepth, float farDepth,
                      float margin, ReachPlane out[6])
{
	float camDepth = Dot(cam.fwd, cam.pos);
	float t = cam.tanHalfFovY;
	float at = cam.aspect * cam.tanHalfFovY;

	// depth = fwd.(p - pos); near: depth - nearDepth >= 0, far: farDepth - depth >= 0.
	for (int k = 0; k < 3; ++k)
	{
		out[0].n[k] = cam.fwd[k];
		out[1].n[k] = -cam.fwd[k];
	}
	out[0].c = -camDepth - nearDepth + margin;
	out[1].c = camDepth + farDepth + margin;

	// |up.(p - pos)| <= t * depth and |right.(p - pos)| <= at * depth.
	SidePlane(cam, t, cam.fwd, -1.0f, cam.up, margin, &out[2]);
	SidePlane(cam, t, cam.fwd, 1.0f, cam.up, margin, &out[3]);
	SidePlane(cam, at, cam.fwd, -1.0f, cam.right, margin, &out[4]);
	SidePlane(cam, at, cam.fwd, 1.0f, cam.right, margin, &out[5]);
}

int ExtrudeTowardLight(const ReachPlane* in, int count, const float L[3],
                       ReachPlane* out, int cap)
{
	int n = 0;
	for (int i = 0; i < count; ++i)
	{
		if (Dot(in[i].n, L) > 0.0f)
			continue;
		if (n >= cap)
			return -1;
		out[n++] = in[i];
	}
	for (int j = 0; j < count; ++j)
	{
		float aj = Dot(in[j].n, L);
		if (!(aj > 0.0f))
			continue;
		for (int i = 0; i < count; ++i)
		{
			float ai = Dot(in[i].n, L);
			if (!(ai < 0.0f))
				continue;
			if (n >= cap)
				return -1;
			ReachPlane& p = out[n++];
			for (int k = 0; k < 3; ++k)
				p.n[k] = aj * in[i].n[k] - ai * in[j].n[k];
			p.c = aj * in[i].c - ai * in[j].c;
		}
	}
	return n;
}

void ShiftPlanes(ReachPlane* planes, int count, const float dir[3], float d)
{
	for (int i = 0; i < count; ++i)
		planes[i].c -= d * Dot(planes[i].n, dir);
}

bool AabbOutsideAnyPlane(const ReachPlane* planes, int count,
                         const float center[3], const float halfSize[3], float grow)
{
	float h[3] = { halfSize[0] + grow, halfSize[1] + grow, halfSize[2] + grow };
	for (int i = 0; i < count; ++i)
	{
		const ReachPlane& p = planes[i];
		float r = fabsf(p.n[0]) * h[0] + fabsf(p.n[1]) * h[1] + fabsf(p.n[2]) * h[2];
		if (Dot(p.n, center) + p.c + r < 0.0f)
			return true;
	}
	return false;
}

bool AabbIsFinite(const float center[3], const float halfSize[3])
{
	for (int k = 0; k < 3; ++k)
	{
		if (!_finite(center[k]) || !_finite(halfSize[k]))
			return false;
	}
	return true;
}

float CascadeBoxSide(float n, float f, float tanHalfFovY, float aspect)
{
	float at = aspect * tanHalfFovY;
	float K = at * at + tanHalfFovY + tanHalfFovY;
	float a = 2.0f * f * sqrtf(K);
	float b = sqrtf((f + n) * (f + n) * K + (f - n) * (f - n));
	return a > b ? a : b;
}

float FrustumDiameter(float f, float tanHalfFovY, float aspect)
{
	float at = aspect * tanHalfFovY;
	float across = 2.0f * f * sqrtf(at * at + tanHalfFovY * tanHalfFovY);
	float along = f * sqrtf(1.0f + at * at + tanHalfFovY * tanHalfFovY);
	return across > along ? across : along;
}

void SunElevationAzimuth(const float L[3], const float fwd[3], const float right[3],
                         float* elevationDeg, float* azimuthDeg)
{
	const float RAD2DEG = 57.2957795f;
	float sun[3] = { -L[0], -L[1], -L[2] };
	float len = sqrtf(Dot(sun, sun));
	float y = len > 0.0f ? sun[1] / len : 0.0f;
	if (y > 1.0f) y = 1.0f;
	if (y < -1.0f) y = -1.0f;
	*elevationDeg = asinf(y) * RAD2DEG;
	// Horizontal components only (x and z), each heading axis normalized so a
	// pitched camera does not skew the angle.
	float fLen = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
	float rLen = sqrtf(right[0] * right[0] + right[2] * right[2]);
	float ahead = fLen > 0.0f ? (sun[0] * fwd[0] + sun[2] * fwd[2]) / fLen : 0.0f;
	float side  = rLen > 0.0f ? (sun[0] * right[0] + sun[2] * right[2]) / rLen : 0.0f;
	*azimuthDeg = (ahead == 0.0f && side == 0.0f) ? 0.0f : atan2f(side, ahead) * RAD2DEG;
}

bool LightTravelDirection(const float towardSun[3], float L[3])
{
	float len = sqrtf(Dot(towardSun, towardSun));
	if (!(len > 1e-6f) || !_finite(len))
		return false;
	for (int k = 0; k < 3; ++k)
		L[k] = -towardSun[k] / len;
	return true;
}

void QuaternionForward(const float wxyz[4], float fwd[3])
{
	float w = wxyz[0], x = wxyz[1], y = wxyz[2], z = wxyz[3];
	// The rotated +z axis, negated.
	fwd[0] = -2.0f * (x * z + w * y);
	fwd[1] = -2.0f * (y * z - w * x);
	fwd[2] = -(1.0f - 2.0f * (x * x + y * y));
}

float AabbMinDepth(const float pos[3], const float fwd[3], const float center[3], const float halfSize[3])
{
	float d[3] = { center[0] - pos[0], center[1] - pos[1], center[2] - pos[2] };
	return Dot(fwd, d) - (fabsf(fwd[0]) * halfSize[0] + fabsf(fwd[1]) * halfSize[1] + fabsf(fwd[2]) * halfSize[2]);
}

unsigned CascadeSkipMask(const float* splits, int cascades, float dmin, float margin)
{
	if (!splits || cascades < 1 || cascades > 8 || !_finite(dmin))
		return 0;
	unsigned mask = 0;
	for (int i = 0; i < cascades; ++i)
	{
		if (splits[i + 1] + margin < dmin)
			mask |= 1u << i;
	}
	return mask;
}

int FloatOrderKey(float v)
{
	int bits;
	memcpy(&bits, &v, sizeof(bits));
	// Negative floats order backwards by magnitude: flip their value bits.
	return bits < 0 ? bits ^ 0x7FFFFFFF : bits;
}

float FloatFromOrderKey(int key)
{
	int bits = key < 0 ? key ^ 0x7FFFFFFF : key;
	float v;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

size_t FilterReach(void** data, size_t begin, size_t end, bool compact,
                   ReachJudge judge, void* ctx, ReachTally* tally)
{
	ReachTally t = { 0, 0, 0, 0, 0 };
	size_t w = begin;
	for (size_t k = begin; k < end; ++k)
	{
		void* obj = data[k];
		bool keep = true;
		if (obj)
		{
			++t.in;
			switch (judge(obj, ctx))
			{
			case RV_CUT:           ++t.cut; keep = false; break;
			case RV_KEEP_INFINITE: ++t.inf; ++t.reach; break;
			case RV_KEEP_BIG:      ++t.big; ++t.reach; break;
			default:               ++t.reach; break;
			}
		}
		if (compact && keep)
			data[w++] = obj;
	}
	*tally = t;
	return compact ? w : end;
}
