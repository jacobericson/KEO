// paused_skip.cpp - The paused off-screen skip: the entry detour on Character::pausedUpdate, its
// install step, and the main-thread tick that hands it the key, advances the frame counter and
// writes the PausedSkip: line. Off, the detour is one read and a forward; on, it reads six bytes,
// counts the verdict and either forwards or calls the character's own load check. No lock, no
// allocation, no logging in the detour.
#include "fixes/world/paused_skip.h"

#ifdef KEO_DEBUG

#include "fixes/world/paused_skip_policy.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include "fixes/fixes_config.h"
#include <sstream>
#include <string>

// GameWorld::paused: counts the paused frames for the heartbeat.
static const size_t PSK_GAMEWORLD_PAUSED = 0x8B9;

typedef void (*pausedUpdate_t)(void* ch);
typedef void (*loadUnloadCheck_t)(void* ch);

// Main thread only. The detour's one caller, GameWorld::charsUpdatePaused, runs on the main
// thread, as the tick does, so no counter or published word needs an atomic operation.
static pausedUpdate_t       orig_paused = NULL;
static PausedSkipPlayerFn   s_isPlayer = NULL;
static const unsigned char* s_paused = NULL;
static int                  s_mode = 0;
static unsigned long        s_frame = 0;
static LONG                 s_count[PSK_VERDICTS] = { 0 };
static LONG                 s_pausedFrames = 0;
static int                  s_seenMode = 0;
static const char*          s_installWhy = "not run";
static double               s_lastBeat = 0;

static void hook_pausedUpdate(void* chv)
{
	if (!s_mode)
	{
		orig_paused(chv);
		return;
	}

	const unsigned char* ch = (const unsigned char*)chv;
	const unsigned char* animation = *(const unsigned char* const*)(ch + PSK_CHAR_ANIMATION);
	PausedSkipInputs in;
	in.visUpdate = ch[PSK_CHAR_VIS_UPDATE];
	in.onScreen = ch[PSK_CHAR_ON_SCREEN];
	in.carried = ch[PSK_CHAR_CARRIED];
	in.haveAnimation = animation != NULL;
	in.actionSlave = animation ? animation[PSK_ANIM_ACTION_SLAVE] : 0;
	in.dead = ch[PSK_CHAR_DEAD];

	const PausedSkipVerdict v = PausedSkipDecide(in, chv, s_frame, s_isPlayer);
	++s_count[v];
	if (v != PSK_SKIP)
	{
		orig_paused(chv);
		return;
	}
	// The original's own tail: the character's zone presence check, and nothing else.
	((loadUnloadCheck_t)(*(void***)chv)[PSK_VT_LOAD_UNLOAD_CHECK / sizeof(void*)])(chv);
}

void InstallPausedSkip(int* installed, int*)
{
	void* isPlayer = GameAddr(RVA_CHARACTER_IS_PLAYER_CHARACTER);
	unsigned char* world = (unsigned char*)GameAddr(RVA_GLOBAL_GAMEWORLD);
	const char* why = NULL;
	if (!isPlayer || !world)
	{
		why = "address";
	}
	else
	{
		s_isPlayer = (PausedSkipPlayerFn)isPlayer;
		s_paused = world + PSK_GAMEWORLD_PAUSED;
		why = HookInstall(HOOK_CHARACTER_PAUSED_UPDATE, hook_pausedUpdate, &orig_paused, installed, true);
	}
	s_installWhy = why;
	if (why)
		ErrorLog(std::string("PausedSkip: install=refused(") + why + ")");
	else
		LogMsg("PausedSkip: install=ok");
}

static void PskHeartbeat(double now)
{
	long long calls = 0;
	for (int i = 0; i < PSK_VERDICTS; ++i)
		calls += s_count[i];
	std::ostringstream ss;
	ss << "PausedSkip: mode=" << (s_seenMode ? "on" : "off");
	if (s_installWhy)
		ss << " install=refused(" << s_installWhy << ")";
	else
		ss << " install=ok";
	ss << " frames=" << s_pausedFrames << " calls=" << calls << " skipped=" << s_count[PSK_SKIP]
	   << " visible=" << s_count[PSK_RUN_VISIBLE] << " kept=" << s_count[PSK_RUN_KEPT]
	   << " players=" << s_count[PSK_RUN_PLAYER] << " refreshed=" << s_count[PSK_RUN_DUE];
	LogMsg(ss.str());
	s_lastBeat = now;
}

void PausedSkipTick(double now)
{
	// Every frame whatever the mode, so a character's turn moves on each paused frame.
	++s_frame;
	if (s_paused && *s_paused)
		++s_pausedFrames;
	const int mode = fixes::g_fixesCfg.cfg_pausedOffscreenSkip ? 1 : 0;
	if (mode != s_seenMode)
	{
		s_mode = mode;
		s_seenMode = mode;
		PskHeartbeat(now);
	}
	else if (now - s_lastBeat >= 60.0)
	{
		PskHeartbeat(now);
	}
}

#else  // !KEO_DEBUG

void InstallPausedSkip(int* installed, int*) { (void)installed; }
void PausedSkipTick(double now) { (void)now; }

#endif // KEO_DEBUG
