#ifndef KEO_FIXES_PAUSED_SKIP_POLICY_H
#define KEO_FIXES_PAUSED_SKIP_POLICY_H

// Which paused per-character updates may be reduced to the character's load
// check while the game is paused. Pure: no game or Windows header.

#include <stddef.h>
#include <stdint.h>

// A Character's fields the decision reads: the visible-update and on-screen
// bytes the AI thread's on-screen check writes, the carried byte, the
// animation, the death byte; the animation's action-slave byte; the Character
// vtable slot of the load check the paused update ends with.
const size_t PSK_CHAR_VIS_UPDATE      = 0xE4;
const size_t PSK_CHAR_ON_SCREEN       = 0x1A9;
const size_t PSK_CHAR_CARRIED         = 0x3D4;
const size_t PSK_CHAR_ANIMATION       = 0x448;
const size_t PSK_CHAR_DEAD            = 0x5BC;
const size_t PSK_ANIM_ACTION_SLAVE    = 0x1A8;
const size_t PSK_VT_LOAD_UNLOAD_CHECK = 0x220;

// A skippable character still gets the whole update one paused frame in this
// many; a power of two.
const unsigned PSK_PERIOD = 16;

struct PausedSkipInputs
{
	unsigned char visUpdate;
	unsigned char onScreen;
	unsigned char carried;
	bool          haveAnimation;
	unsigned char actionSlave;   // 0 when there is no animation
	unsigned char dead;
};

enum PausedSkipVerdict
{
	PSK_RUN_VISIBLE,   // on screen or in visible-update mode
	PSK_RUN_KEPT,      // carried, an action slave, or no animation
	PSK_RUN_PLAYER,    // the update leases the player character's cell
	PSK_RUN_DUE,       // its turn in the period
	PSK_SKIP,
	PSK_VERDICTS
};

typedef bool (*PausedSkipPlayerFn)(void* ch);

// The verdict, in the order of the enum. isPlayer is called only for a living
// character that neither of the first two verdicts took.
PausedSkipVerdict PausedSkipDecide(const PausedSkipInputs& in, void* ch, unsigned long frame,
                                   PausedSkipPlayerFn isPlayer);

// ch's turn: ((frame + ((ch >> 4) ^ (ch >> 12))) % PSK_PERIOD) == 0.
bool PausedSkipDue(uintptr_t ch, unsigned long frame);

#endif // KEO_FIXES_PAUSED_SKIP_POLICY_H
