#ifndef KEO_FIXES_ONSCREEN_STAGGER_POLICY_H
#define KEO_FIXES_ONSCREEN_STAGGER_POLICY_H

// When the on-screen check of a far, off-screen character may be answered
// without the engine's full check, and the far branch's writes it then makes.
// Pure: no game or Windows header.

#include <stddef.h>
#include <stdint.h>

// A Character's fields: its height, the off-screen time the check adds to,
// the visible-update word (isVisibleUpdateMode, then setVisibleMsg), the
// visible-and-near and on-screen bytes, the engaged-with-a-player byte and
// its animation; the animation's blend reset byte; the race's gigantic byte;
// the faction's player interface; the two Character vtable slots called.
const size_t ONS_CHAR_POS_Y          = 0x4C;
const size_t ONS_CHAR_OFFSCREEN_TIME = 0xC0;
const size_t ONS_CHAR_VIS_WORD       = 0xE4;
const size_t ONS_CHAR_VISIBLE_NEAR   = 0x1A8;
const size_t ONS_CHAR_ON_SCREEN      = 0x1A9;
const size_t ONS_CHAR_ENGAGED        = 0x250;
const size_t ONS_CHAR_ANIMATION      = 0x448;
const size_t ONS_ANIM_ZERO_BLEND     = 0x8;
const size_t ONS_RACE_GIGANTIC       = 0x78;
const size_t ONS_FACTION_PLAYER      = 0x250;
const size_t ONS_VT_GET_FACTION      = 0x58;
const size_t ONS_VT_GET_RACE         = 0xD0;

const unsigned ONS_STRIPE = 4;   // a far character's full check comes one run in this many

// The reads that decide without calling the engine.
struct OnScreenCheap
{
	bool           forced;        // a camera jump or a save load asked for a full pass
	bool           stripeDue;
	unsigned short visWord;       // +0xE4 and +0xE5 together
	unsigned char  onScreen;
	unsigned char  engaged;
	float          posY;
	bool           haveAnimation;
};

// True: the engine's own check runs.
bool OnScreenNeedsFull(const OnScreenCheap& c);

// A character's due run: ((ch >> 6) + frame) % ONS_STRIPE == 0.
bool OnScreenStripeDue(uintptr_t ch, long frame);

// True when frame has not passed forceUntil (signed, so the counters may wrap).
bool OnScreenForced(long frame, long forceUntil);

// Beyond twice NPCRange from the camera; false for a range that is not a
// positive number or a distance that is not a number.
bool OnScreenFarEnough(float distSq, float npcRange);

// The far branch's writes, in the engine's order: the visible-update word 0,
// on screen 0, the off-screen time advanced by frameTime unless paused, the
// animation's blend reset 1, visible-and-near 0.
void OnScreenFarWrites(unsigned char* ch, unsigned char* animation, bool paused, float frameTime);

// The camera centre and its cell (-1 when the grid is not known yet).
struct OnScreenCam
{
	bool  valid;
	float x, z;
	int   cellX, cellY;
};

enum OnScreenCamVerdict { CAM_STEADY, CAM_JUMP, CAM_UNKNOWN };

// UNKNOWN: no baseline or no read; JUMP: the cell changed (both known), or the
// centre moved more than half NPCRange in x-z; else STEADY.
OnScreenCamVerdict OnScreenCameraJump(const OnScreenCam& last, const OnScreenCam& now, float npcRange);

#endif // KEO_FIXES_ONSCREEN_STAGGER_POLICY_H
