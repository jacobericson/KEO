#include <climits>
#include <cstring>
#include <limits>
#include "fixes/world/onscreen_stagger_policy.h"

#include "check.h"

static const size_t CHAR_BYTES = 0x460;
static const size_t ANIM_BYTES = 0x20;

static unsigned FloatBits(float f)
{
	unsigned u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

static float ReadFloat(const unsigned char* p)
{
	float f;
	std::memcpy(&f, p, sizeof(f));
	return f;
}

static void WriteFloat(unsigned char* p, float f)
{
	std::memcpy(p, &f, sizeof(f));
}

// A far candidate that every cheap read lets through.
static OnScreenCheap Quiet()
{
	OnScreenCheap c;
	c.forced = false;
	c.stripeDue = false;
	c.visWord = 0;
	c.onScreen = 0;
	c.engaged = 0;
	c.posY = 12.5f;
	c.haveAnimation = true;
	return c;
}

static void CheckNeedsFull()
{
	Check(!OnScreenNeedsFull(Quiet()), "full: a quiet far candidate goes on to the distance");

	OnScreenCheap c = Quiet();
	c.forced = true;
	Check(OnScreenNeedsFull(c), "full: a forced run checks everyone");
	c = Quiet();
	c.stripeDue = true;
	Check(OnScreenNeedsFull(c), "full: a due stripe checks");
	c = Quiet();
	c.onScreen = 1;
	Check(OnScreenNeedsFull(c), "full: a character last on screen is checked");
	c = Quiet();
	c.visWord = 0x0001;
	Check(OnScreenNeedsFull(c), "full: a visible-update character is checked");
	c = Quiet();
	c.visWord = 0x0100;
	Check(OnScreenNeedsFull(c), "full: a set-visible message alone is checked");
	c = Quiet();
	c.engaged = 1;
	Check(OnScreenNeedsFull(c), "full: an engaged character is checked");
	c = Quiet();
	c.posY = -0.5f;
	Check(OnScreenNeedsFull(c), "full: a negative height is checked");
	c = Quiet();
	c.posY = std::numeric_limits<float>::quiet_NaN();
	Check(OnScreenNeedsFull(c), "full: a height that is not a number is checked");
	c = Quiet();
	c.haveAnimation = false;
	Check(OnScreenNeedsFull(c), "full: no animation is checked");
}

static void CheckStripe()
{
	static const long kStarts[] = { 0, 1001 };
	bool ok = true;
	for (int i = 0; i < 64; ++i)
	{
		const uintptr_t ch = (uintptr_t)(0x10000 + i * 0x6F0);
		for (int s = 0; s < 2; ++s)
		{
			int due = 0;
			for (long f = kStarts[s]; f < kStarts[s] + 4; ++f)
				if (OnScreenStripeDue(ch, f))
					++due;
			ok = ok && due == 1;
		}
	}
	Check(ok, "stripe: every character is due exactly once in four consecutive frames");
}

static void CheckForced()
{
	Check(OnScreenForced(10, 10) && !OnScreenForced(11, 10),
	      "forced: the frame at the limit is forced, the one after is not");
	Check(OnScreenForced(LONG_MAX, LONG_MAX) && OnScreenForced(LONG_MAX, LONG_MIN + 1)
	      && !OnScreenForced(LONG_MIN + 2, LONG_MIN + 1),
	      "forced: a wrapped counter keeps the order");
}

static void CheckFar()
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(OnScreenFarEnough(40001.0f, 100.0f), "far: beyond twice the range is far");
	Check(!OnScreenFarEnough(40000.0f, 100.0f), "far: exactly twice the range is not");
	Check(!OnScreenFarEnough(20000.0f, 100.0f), "far: between the range and twice the range is not");
	Check(!OnScreenFarEnough(5000.0f, 100.0f), "far: inside the range is not");
	Check(!OnScreenFarEnough(1.0e9f, 0.0f) && !OnScreenFarEnough(1.0e9f, -100.0f),
	      "far: a zero or negative range is not");
	Check(!OnScreenFarEnough(nan, 100.0f) && !OnScreenFarEnough(1.0e9f, nan),
	      "far: a distance or range that is not a number is not");
}

static bool OutsideFarFields(size_t i)
{
	return i != ONS_CHAR_VIS_WORD && i != ONS_CHAR_VIS_WORD + 1 && i != ONS_CHAR_ON_SCREEN
	    && i != ONS_CHAR_VISIBLE_NEAR
	    && (i < ONS_CHAR_OFFSCREEN_TIME || i >= ONS_CHAR_OFFSCREEN_TIME + 4);
}

static void CheckWrites()
{
	unsigned char ch[CHAR_BYTES];
	unsigned char anim[ANIM_BYTES];
	unsigned char chBefore[CHAR_BYTES];
	unsigned char animBefore[ANIM_BYTES];
	std::memset(ch, 0xAB, sizeof(ch));
	std::memset(anim, 0xAB, sizeof(anim));
	WriteFloat(ch + ONS_CHAR_OFFSCREEN_TIME, 3.3f);
	std::memcpy(chBefore, ch, sizeof(ch));
	std::memcpy(animBefore, anim, sizeof(anim));

	OnScreenFarWrites(ch, anim, false, 0.1f);

	unsigned short word;
	std::memcpy(&word, ch + ONS_CHAR_VIS_WORD, sizeof(word));
	Check(word == 0 && ch[ONS_CHAR_ON_SCREEN] == 0 && anim[ONS_ANIM_ZERO_BLEND] == 1
	      && ch[ONS_CHAR_VISIBLE_NEAR] == 0,
	      "writes: the far branch's five writes");

	volatile float dt = 0.1f;
	volatile float old = 3.3f;
	const float want = dt + old;
	Check(FloatBits(ReadFloat(ch + ONS_CHAR_OFFSCREEN_TIME)) == FloatBits(want),
	      "writes: the add is frameTime plus the old value");

	bool same = true;
	for (size_t i = 0; i < CHAR_BYTES; ++i)
		if (OutsideFarFields(i))
			same = same && ch[i] == chBefore[i];
	for (size_t i = 0; i < ANIM_BYTES; ++i)
		if (i != ONS_ANIM_ZERO_BLEND)
			same = same && anim[i] == animBefore[i];

	std::memcpy(ch, chBefore, sizeof(ch));
	std::memcpy(anim, animBefore, sizeof(anim));
	OnScreenFarWrites(ch, anim, true, 0.1f);
	Check(FloatBits(ReadFloat(ch + ONS_CHAR_OFFSCREEN_TIME)) == FloatBits(3.3f),
	      "writes: paused keeps the off-screen time");
	for (size_t i = 0; i < CHAR_BYTES; ++i)
		if (OutsideFarFields(i))
			same = same && ch[i] == chBefore[i];
	for (size_t i = 0; i < ANIM_BYTES; ++i)
		if (i != ONS_ANIM_ZERO_BLEND)
			same = same && anim[i] == animBefore[i];
	Check(same, "writes: nothing else changes");
}

static OnScreenCam Cam(bool valid, float x, float z, int cellX, int cellY)
{
	OnScreenCam c;
	c.valid = valid;
	c.x = x;
	c.z = z;
	c.cellX = cellX;
	c.cellY = cellY;
	return c;
}

static void CheckCamera()
{
	const float r = 1000.0f;
	const OnScreenCam base = Cam(true, 5000.0f, 7000.0f, 10, 12);
	Check(OnScreenCameraJump(Cam(false, 0.0f, 0.0f, -1, -1), base, r) == CAM_UNKNOWN,
	      "camera: no baseline is unknown");
	Check(OnScreenCameraJump(base, Cam(false, 0.0f, 0.0f, -1, -1), r) == CAM_UNKNOWN,
	      "camera: a failed read is unknown");
	Check(OnScreenCameraJump(base, Cam(true, 5000.0f, 7000.0f, 11, 12), r) == CAM_JUMP
	      && OnScreenCameraJump(base, Cam(true, 5000.0f, 7000.0f, 10, 13), r) == CAM_JUMP,
	      "camera: a cell change is a jump");
	Check(OnScreenCameraJump(base, Cam(true, 5501.0f, 7000.0f, 10, 12), r) == CAM_JUMP,
	      "camera: a move above half the range is a jump");
	Check(OnScreenCameraJump(base, Cam(true, 5000.0f, 7499.0f, 10, 12), r) == CAM_STEADY,
	      "camera: a small move in one cell is steady");
	const OnScreenCam unknownCell = Cam(true, 5000.0f, 7000.0f, -1, -1);
	Check(OnScreenCameraJump(unknownCell, Cam(true, 5501.0f, 7000.0f, -1, -1), r) == CAM_JUMP
	      && OnScreenCameraJump(unknownCell, Cam(true, 5499.0f, 7000.0f, -1, -1), r) == CAM_STEADY
	      && OnScreenCameraJump(unknownCell, Cam(true, 5000.0f, 7000.0f, 10, 12), r) == CAM_STEADY,
	      "camera: an unknown cell uses the distance alone");
}

int main()
{
	CheckNeedsFull();
	CheckStripe();
	CheckForced();
	CheckFar();
	CheckWrites();
	CheckCamera();
	return CheckExit("onscreen_stagger_units");
}
