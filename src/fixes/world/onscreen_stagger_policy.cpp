// onscreen_stagger_policy.cpp - The far visibility-check stagger's pure rules.
#include "fixes/world/onscreen_stagger_policy.h"
#include <float.h>

// Any reason to run the engine's check: a forced pass, a due stripe, a
// character the last check left visible in any way, an engaged one, a height
// the engine would snap (or not a number), or no animation to write to.
bool OnScreenNeedsFull(const OnScreenCheap& c)
{
	return c.forced || c.stripeDue || c.visWord != 0 || c.onScreen != 0 || c.engaged != 0
	    || !(c.posY >= 0.0f) || !c.haveAnimation;
}

bool OnScreenStripeDue(uintptr_t ch, long frame)
{
	return (((unsigned long long)ch >> 6) + (unsigned long long)(unsigned long)frame) % ONS_STRIPE == 0;
}

// The difference taken in unsigned arithmetic, then read signed: the order
// survives a wrap of either counter.
bool OnScreenForced(long frame, long forceUntil)
{
	return (long)((unsigned long)forceUntil - (unsigned long)frame) >= 0;
}

bool OnScreenFarEnough(float distSq, float npcRange)
{
	if (!(npcRange > 0.0f) || !(npcRange == npcRange && npcRange < FLT_MAX))
		return false;
	if (!(distSq == distSq))
		return false;
	const float twice = 2.0f * npcRange;
	return distSq > twice * twice;
}

void OnScreenFarWrites(unsigned char* ch, unsigned char* animation, bool paused, float frameTime)
{
	*(unsigned short*)(ch + ONS_CHAR_VIS_WORD) = 0;
	ch[ONS_CHAR_ON_SCREEN] = 0;
	if (!paused)
	{
		float* t = (float*)(ch + ONS_CHAR_OFFSCREEN_TIME);
		*t = frameTime + *t;
	}
	animation[ONS_ANIM_ZERO_BLEND] = 1;
	ch[ONS_CHAR_VISIBLE_NEAR] = 0;
}

OnScreenCamVerdict OnScreenCameraJump(const OnScreenCam& last, const OnScreenCam& now, float npcRange)
{
	if (!last.valid || !now.valid)
		return CAM_UNKNOWN;
	const bool cellsKnown = last.cellX >= 0 && last.cellY >= 0 && now.cellX >= 0 && now.cellY >= 0;
	if (cellsKnown && (last.cellX != now.cellX || last.cellY != now.cellY))
		return CAM_JUMP;
	// A range that is not a positive number gives no distance rule.
	if (npcRange > 0.0f && npcRange == npcRange && npcRange < FLT_MAX)
	{
		const float dx = now.x - last.x;
		const float dz = now.z - last.z;
		const float half = 0.5f * npcRange;
		if (dx * dx + dz * dz > half * half)
			return CAM_JUMP;
	}
	return CAM_STEADY;
}
