#include "bench/bench_game_math.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

bool BenchRipTarget(const unsigned char* code, size_t insnOff, const unsigned char* opcode,
                    size_t opLen, uintptr_t codeAddr, uintptr_t* target)
{
	if (memcmp(code + insnOff, opcode, opLen) != 0)
		return false;
	int disp;
	memcpy(&disp, code + insnOff + opLen, sizeof(disp));
	*target = codeAddr + insnOff + opLen + sizeof(disp) + (intptr_t)disp;
	return true;
}

bool BenchNormalizeQuat(float q[4])
{
	float len2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if (!_finite(len2) || len2 < 1e-6f)
		return false;
	float inv = 1.0f / sqrtf(len2);
	for (int i = 0; i < 4; ++i)
		q[i] *= inv;
	return true;
}

float BenchClampZoom(float zoom)
{
	float d = fabsf(zoom);
	if (!(d >= 10.0f))
		d = 10.0f;
	else if (d > 2000.0f)
		d = 2000.0f;
	return zoom > 0.0f ? d : -d;
}

float BenchDistance3(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

float BenchQuatAngleDeg(const float a[4], const float b[4])
{
	float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
	float lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
	if (!(la > 1e-6f) || !(lb > 1e-6f))
		return 180.0f;
	float dot = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (la * lb);
	if (!(dot < 1.0f))
		return 0.0f;
	return 2.0f * acosf(dot) * 57.29578f;
}

void BenchPoseDrift(const float targetPos[3], const float targetRot[4], float targetZoom,
                    const float pos[3], const float rot[4], float zoom,
                    float* units, float* deg)
{
	float dx = pos[0] - targetPos[0], dz = pos[2] - targetPos[2];
	float d = sqrtf(dx * dx + dz * dz);
	float dZoom = fabsf(zoom - targetZoom);
	*units = dZoom > d ? dZoom : d;
	*deg = BenchQuatAngleDeg(targetRot, rot);
}

std::string FormatBenchWeather(const std::string& name, float strength, float wind)
{
	std::string n = name.empty() ? std::string("none") : name;
	for (size_t i = 0; i < n.size(); ++i)
	{
		if (n[i] == ' ')
			n[i] = '_';
	}
	char buf[64];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, " %.2f wind %.1f", strength, wind);
	return n + buf;
}
