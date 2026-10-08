// onscreen_stagger.cpp - The far visibility-check stagger: the entry detour on
// Character::updateOnScreenCheck, its install step, and the main-thread tick that hands it the key,
// advances the frame counter and forces a full pass on a camera jump or a save load. DEV builds
// record counters and write the OnScreenStagger: line. The detour runs on the AI back thread:
// one volatile read and a forward when off, no lock, no allocation, no logging.
#include "fixes/world/onscreen_stagger.h"

#include "fixes/world/onscreen_stagger_policy.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include "game/game.h"
#include "zone/zone_helpers.h"
#include "zone/grid.h"
#include "fixes/fixes_config.h"
#include <float.h>
#include <sstream>
#include <string>

KLIB_ASSERT_OFFSET(RootObjectBase_pos_y, ONS_CHAR_POS_Y);

// RootObjectBase::pos, the Vector3 whose y is ONS_CHAR_POS_Y; the check passes its address to
// the camera distance.
static const size_t ONS_CHAR_POS = 0x48;
// GameWorld::paused: while it is set the check leaves the off-screen time as it is.
static const size_t ONS_GAMEWORLD_PAUSED = 0x8B9;

typedef bool (*onScreenCheck_t)(void* ch);
typedef float (*sqDistFromCamera_t)(void* player, const void* point);

static onScreenCheck_t    orig_check = NULL;
static sqDistFromCamera_t s_sqDist = NULL;

// Resolved once at install; the detour reads through them.
static void* const*         s_playerSlot = NULL;
static const float*         s_npcRange = NULL;
static const unsigned char* s_paused = NULL;
static const float*         s_frameTime = NULL;

// Published by the tick, read by the detour.
static volatile LONG s_mode = 0;
static volatile LONG s_frame = 0;
static volatile LONG s_forceUntil = 0;

#ifdef KEO_DEBUG
static volatile LONG s_full = 0;
static volatile LONG s_skipped = 0;
static volatile LONG s_jumps = 0;
#endif

// Main thread only.
static int         s_seenMode = 0;
#ifdef KEO_DEBUG
static const char* s_installWhy = "not run";
#endif
static OnScreenCam s_last = { false, 0.0f, 0.0f, -1, -1 };
#ifdef KEO_DEBUG
static double      s_lastBeat = 0;
#endif

static void* OnsVcall(void* obj, size_t slot)
{
	typedef void* (*getter_t)(void*);
	return ((getter_t)(*(void***)obj)[slot / sizeof(void*)])(obj);
}

static bool OnsFull(void* chv)
{
#ifdef KEO_DEBUG
	InterlockedIncrement(&s_full);
#endif
	return orig_check(chv);
}

static bool hook_updateOnScreenCheck(void* chv)
{
	if (!s_mode)
		return orig_check(chv);

	unsigned char* ch = (unsigned char*)chv;
	unsigned char* animation = *(unsigned char**)(ch + ONS_CHAR_ANIMATION);
	const long frame = (long)s_frame;
	OnScreenCheap c;
	c.forced = OnScreenForced(frame, (long)s_forceUntil);
	c.stripeDue = OnScreenStripeDue((uintptr_t)ch, frame);
	c.visWord = *(const unsigned short*)(ch + ONS_CHAR_VIS_WORD);
	c.onScreen = ch[ONS_CHAR_ON_SCREEN];
	c.engaged = ch[ONS_CHAR_ENGAGED];
	c.posY = *(const float*)(ch + ONS_CHAR_POS_Y);
	c.haveAnimation = animation != NULL;
	if (OnScreenNeedsFull(c))
		return OnsFull(chv);

	// The player faction is immune to the off-screen mode; a gigantic race skips the range test.
	const unsigned char* faction = (const unsigned char*)OnsVcall(chv, ONS_VT_GET_FACTION);
	if (faction && *(void* const*)(faction + ONS_FACTION_PLAYER))
		return OnsFull(chv);
	const unsigned char* race = (const unsigned char*)OnsVcall(chv, ONS_VT_GET_RACE);
	if (!race || race[ONS_RACE_GIGANTIC])
		return OnsFull(chv);
	void* player = *s_playerSlot;
	if (!player)
		return OnsFull(chv);
	if (!OnScreenFarEnough(s_sqDist(player, ch + ONS_CHAR_POS), *s_npcRange))
		return OnsFull(chv);

	OnScreenFarWrites(ch, animation, *s_paused != 0, *s_frameTime);
#ifdef KEO_DEBUG
	InterlockedIncrement(&s_skipped);
#endif
	return false;
}

void InstallOnScreenStagger(int* installed, int*)
{
	void* playerSlot = GameAddr(RVA_GLOBAL_PLAYER);
	void* options = GameAddr(RVA_GLOBAL_OPTIONS);
	unsigned char* world = (unsigned char*)GameAddr(RVA_GLOBAL_GAMEWORLD);
	void* frameTime = GameAddr(RVA_GLOBAL_GAME_FRAME_TIME);
	void* sqDist = GameAddr(RVA_PLAYER_SQ_DIST_FROM_CAMERA);
	const char* why = NULL;
	if (!playerSlot || !options || !world || !frameTime || !sqDist)
	{
		why = "address";
	}
	else
	{
		s_playerSlot = (void* const*)playerSlot;
		s_npcRange = (const float*)KLIB_MEMBER(5, options, OptionsHolder_NPCRange, 0x38);
		s_paused = world + ONS_GAMEWORLD_PAUSED;
		s_frameTime = (const float*)frameTime;
		s_sqDist = (sqDistFromCamera_t)sqDist;
		why = HookInstall(HOOK_CHARACTER_UPDATE_ONSCREEN_CHECK, hook_updateOnScreenCheck, &orig_check,
		                  installed, true);
	}
#ifdef KEO_DEBUG
	s_installWhy = why;
#endif
	if (why)
		ErrorLog(std::string("OnScreenStagger: install=refused(") + why + ")");
	else
		LogMsg("OnScreenStagger: install=ok");
}

#ifdef KEO_DEBUG
static LONG OnsRead(volatile LONG* x)
{
	return InterlockedCompareExchange(x, 0, 0);
}
#endif

// The next three AI runs check everyone.
static void OnsForce(LONG frame)
{
	InterlockedExchange(&s_forceUntil, (LONG)((unsigned long)frame + 2u));
}

static float OnsNpcRange()
{
	void* options = GameAddr(RVA_GLOBAL_OPTIONS);
	if (!options)
		return 0.0f;
	return *(const float*)KLIB_MEMBER(5, options, OptionsHolder_NPCRange, 0x38);
}

static OnScreenCam OnsReadCamera()
{
	OnScreenCam cam = { false, 0.0f, 0.0f, -1, -1 };
	void* playerSlot = GameAddr(RVA_GLOBAL_PLAYER);
	if (!playerSlot)
		return cam;
	if (!GetCameraFocusXZ(*(uintptr_t*)playerSlot, &cam.x, &cam.z))
		return cam;
	if (!(cam.x == cam.x && cam.z == cam.z && cam.x < FLT_MAX && cam.x > -FLT_MAX
	      && cam.z < FLT_MAX && cam.z > -FLT_MAX))
		return cam;
	cam.valid = true;
	if (!WorldToZoneGrid(cam.x, cam.z, &cam.cellX, &cam.cellY))
	{
		cam.cellX = -1;
		cam.cellY = -1;
	}
	return cam;
}

#ifdef KEO_DEBUG
static void OnsHeartbeat(double now)
{
	const LONG full = OnsRead(&s_full);
	const LONG skipped = OnsRead(&s_skipped);
	std::ostringstream ss;
	ss << "OnScreenStagger: mode=" << (s_seenMode ? "on" : "off");
	if (s_installWhy)
		ss << " install=refused(" << s_installWhy << ")";
	else
		ss << " install=ok";
	ss << " calls=" << (long long)full + (long long)skipped
	   << " full=" << full << " skipped=" << skipped << " jumps=" << OnsRead(&s_jumps);
	LogMsg(ss.str());
	s_lastBeat = now;
}
#endif

void OnScreenStaggerTick(double now, bool saveLoading)
{
	const LONG frame = InterlockedIncrement(&s_frame);
	const int mode = fixes::g_fixesCfg.cfg_onScreenStagger ? 1 : 0;
#ifdef KEO_DEBUG
	bool beat = false;
#endif
	if (mode != s_seenMode)
	{
		s_last.valid = false;
		OnsForce(frame);
		InterlockedExchange(&s_mode, (LONG)mode);
		s_seenMode = mode;
#ifdef KEO_DEBUG
		beat = true;
#endif
	}

	if (saveLoading)
	{
		s_last.valid = false;
		OnsForce(frame);
	}
	else if (mode)
	{
		const OnScreenCam cam = OnsReadCamera();
		const OnScreenCamVerdict v = OnScreenCameraJump(s_last, cam, OnsNpcRange());
		if (v == CAM_JUMP)
		{
			OnsForce(frame);
#ifdef KEO_DEBUG
			InterlockedIncrement(&s_jumps);
#endif
		}
		else if (v == CAM_UNKNOWN)
		{
			OnsForce(frame);
		}
		s_last = cam;
	}

#ifdef KEO_DEBUG
	if (beat || now - s_lastBeat >= 60.0)
		OnsHeartbeat(now);
#else
	(void)now;
#endif
}
