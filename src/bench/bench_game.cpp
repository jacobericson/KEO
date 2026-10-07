#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bench/bench_game.h"
#include "bench/bench_game_internal.h"
#include "bench/bench_game_math.h"
#include "game/game.h"
#include "base/core.h"
#include "zone/transition.h"
#include <float.h>
#include <stddef.h>
#include <string.h>
#include "base/klib_include.h"
#include <core/Functions.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/CameraClass.h>
#include <kenshi/util/hand.h>
#include <kenshi/InputHandler.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/OptionsWindow.h>
#include "base/klib_include_end.h"

static_assert(offsetof(GameWorld, player) == 0x580, "GameWorld::player");
static_assert(offsetof(GameWorld, frameSpeedMult) == 0x700, "GameWorld::frameSpeedMult");
static_assert(offsetof(GameWorld, zoneMgr) == 0x8B0, "GameWorld::zoneMgr");
static_assert(offsetof(GameWorld, paused) == 0x8B9, "GameWorld::paused");
static_assert(offsetof(PlayerInterface, camera) == 0x30, "PlayerInterface::camera");
static_assert(offsetof(PlayerInterface, trackedCharacterHandle) == 0x270, "PlayerInterface::trackedCharacterHandle");
static_assert(offsetof(hand, type) == 0x8 && sizeof(hand) <= sizeof(((BenchFollow*)0)->handle), "hand fits BenchFollow");
static_assert(offsetof(CameraClass, center) == 0x58, "CameraClass::center");
static_assert(offsetof(CameraClass, node) == 0x70, "CameraClass::node");
static_assert(offsetof(InputHandler, controlEnabled) == 0xD0, "InputHandler::controlEnabled");
static_assert(offsetof(OptionsWindow, mMainWidget) == 0x38, "OptionsWindow::mMainWidget");
static_assert(sizeof(Ogre::Quaternion) == 16 && offsetof(Ogre::Quaternion, w) == 0, "Ogre::Quaternion is w, x, y, z");
static_assert(sizeof(Ogre::Vector3) == 12, "Ogre::Vector3 is x, y, z");

// The sky object (RVA_SKY_INSTANCE) and its SkyXController.
static const size_t SKY_DAY         = 0x08;   // int, raised by the advance when the hour wraps
static const size_t SKY_CONTROLLER  = 0x20;
static const size_t SKY_TOTAL_HOURS = 0xA0;   // double, day * 24 + hour, derived every frame
static const size_t CONTROLLER_HOUR = 0x1C;

// Ogre returns both by value, through a hidden pointer after `this`.
typedef void* (*NodeOrientation_t)(const void* node, float* wxyz);
typedef void* (*NodePosition_t)(const void* node, float* xyz);

static bool              s_ready          = false;
static NodeOrientation_t s_nodeOrient     = NULL;
static NodePosition_t    s_nodePosition   = NULL;
static float*            s_userPauseSpeed = NULL;   // userPause's saved speed and its static-init guard
static unsigned*         s_userPauseGuard = NULL;
static void* const*      s_optionsWindow  = NULL;
static const uintptr_t*  s_skyInstance    = NULL;
static bool              s_followReady    = false;
static float*            s_clockRate      = NULL;   // NULL: the clock pin is off
static float             s_clockDefault   = 0.0f;

bool BenchSameAddress(const void* klibAddr, size_t rva, const char* name)
{
	if (klibAddr == GameAddr(rva))
		return true;
	LogMsg(std::string("Bench: ") + name + " address differs from KenshiLib's");
	return false;
}

bool BenchCheckAnchor(size_t fnRva, size_t insnOff, const unsigned char* opcode, size_t opLen,
                      size_t expectRva, const char* name)
{
	uintptr_t fn = (uintptr_t)GameAddr(fnRva);
	uintptr_t target = 0;
	if (BenchRipTarget((const unsigned char*)fn, insnOff, opcode, opLen, fn, &target) &&
	    target == gameBase + expectRva)
		return true;
	LogMsg(std::string("Bench: ") + name + " is not where the game's code reads it");
	return false;
}

#define BENCH_COVERED(fn, rva) BenchSameAddress((const void*)KlibRealAddress(&fn), rva, #fn)

static bool Fail(std::string* whyNot, const char* reason)
{
	if (whyNot)
		*whyNot = reason;
	LogMsg(std::string("Bench: game facade off (") + reason + ")");
	return false;
}

// The clock rate's only reader, the day's increment beside it, and the shipped
// value (another plugin's patch of the same constant refuses the pin). NULL
// when the pin can run, else why not.
static const char* InstallClock()
{
	static const unsigned char MULSS_XMM0_RIP[] = { 0xF3, 0x0F, 0x59, 0x05 };
	static const unsigned char INC_DAY[]        = { 0xFF, 0x47, 0x08 };
	s_clockRate = NULL;
	if (!BenchCheckAnchor(RVA_SKY_ADVANCE, OFF_SKY_ADVANCE_RATE_READ, MULSS_XMM0_RIP, sizeof(MULSS_XMM0_RIP),
	                      RVA_CLOCK_RATE, "clock rate"))
		return "reader";
	if (memcmp(GameAddr(RVA_SKY_DAY_INCREMENT), INC_DAY, sizeof(INC_DAY)) != 0)
		return "day";
	float* rate = (float*)(gameBase + RVA_CLOCK_RATE);
	unsigned bits;
	memcpy(&bits, rate, sizeof(bits));
	if (bits != CLOCK_RATE_BITS)
		return "value";
	s_clockDefault = *rate;
	s_clockRate = rate;
	return NULL;
}

bool BenchGameInstall(std::string* whyNot)
{
	s_ready = false;
	if ((uintptr_t)ou != gameBase + RVA_GLOBAL_GAMEWORLD || (uintptr_t)gui != gameBase + RVA_GLOBAL_GUI ||
	    (uintptr_t)key != gameBase + RVA_GLOBAL_INPUT)
		return Fail(whyNot, "globals");

	if (!BENCH_COVERED(CameraClass::teleport, RVA_CAMERA_TELEPORT) ||
	    !BENCH_COVERED(CameraClass::manuallySetOrientationAndZoom, RVA_CAMERA_ORIENT_ZOOM) ||
	    !BENCH_COVERED(CameraClass::getCenter, RVA_CAMERA_GET_CENTER))
		return Fail(whyNot, "camera");

	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	s_nodeOrient = ogre ? (NodeOrientation_t)GetProcAddress(ogre, "?getOrientation@Node@Ogre@@QEBA?AVQuaternion@2@XZ") : NULL;
	s_nodePosition = ogre ? (NodePosition_t)GetProcAddress(ogre, "?getPosition@Node@Ogre@@QEBA?AVVector3@2@XZ") : NULL;
	if (!s_nodeOrient || !s_nodePosition)
		return Fail(whyNot, "ogre");

	static const unsigned char MOV_EAX_RIP[]   = { 0x8B, 0x05 };
	static const unsigned char MOVSS_XMM6_RIP[] = { 0xF3, 0x0F, 0x10, 0x35 };
	static const unsigned char MOV_RAX_RIP[]   = { 0x48, 0x8B, 0x05 };
	if (!BENCH_COVERED(GameWorld::setGameSpeed, RVA_GW_SET_GAME_SPEED) ||
	    !BENCH_COVERED(GameWorld::userPause, RVA_GW_USER_PAUSE) ||
	    !BenchCheckAnchor(RVA_GW_USER_PAUSE, 0x14, MOV_EAX_RIP, sizeof(MOV_EAX_RIP), RVA_USER_PAUSE_GUARD, "userPause guard") ||
	    !BenchCheckAnchor(RVA_GW_USER_PAUSE, 0x41, MOVSS_XMM6_RIP, sizeof(MOVSS_XMM6_RIP), RVA_USER_PAUSE_SPEED, "userPause speed"))
		return Fail(whyNot, "speed");

	if (!BENCH_COVERED(ForgottenGUI::isLoading, RVA_GUI_IS_LOADING) ||
	    !BENCH_COVERED(ForgottenGUI::isPaused, RVA_GUI_IS_PAUSED) ||
	    !BENCH_COVERED(OptionsWindow::isVisible, RVA_OPTIONS_IS_VISIBLE) ||
	    !BENCH_COVERED(OptionsWindow::getSingleton, RVA_OPTIONS_GET_SINGLETON) ||
	    !BenchCheckAnchor(RVA_OPTIONS_GET_SINGLETON, 0x47, MOV_RAX_RIP, sizeof(MOV_RAX_RIP), RVA_OPTIONS_INSTANCE, "OptionsWindow instance"))
		return Fail(whyNot, "menus");

	if (!BENCH_COVERED(GameWorld::getTimeStamp_inGameHours, RVA_GW_TIMESTAMP_HOURS) ||
	    !BenchCheckAnchor(RVA_GW_TIMESTAMP_HOURS, 0, MOV_RAX_RIP, sizeof(MOV_RAX_RIP), RVA_SKY_INSTANCE, "sky instance"))
		return Fail(whyNot, "sky");

	s_userPauseSpeed = (float*)(gameBase + RVA_USER_PAUSE_SPEED);
	s_userPauseGuard = (unsigned*)(gameBase + RVA_USER_PAUSE_GUARD);
	s_optionsWindow  = (void* const*)(gameBase + RVA_OPTIONS_INSTANCE);
	s_skyInstance    = (const uintptr_t*)(gameBase + RVA_SKY_INSTANCE);

	if (!BenchWeatherInstall())
		LogMsg("Bench: weather readout off, runs record weather=unknown");
	const char* clockWhy = InstallClock();
	if (clockWhy)
		LogMsg(std::string("Bench: clock pin off (") + clockWhy + ")");
	s_followReady = BENCH_COVERED(PlayerInterface::startTrackCharacter, RVA_PI_START_TRACK_CHARACTER) &&
	                BENCH_COVERED(hand::getRootObject, RVA_HAND_GET_ROOT_OBJECT);
	if (!s_followReady)
		LogMsg("Bench: camera follow off, runs do not restore it");
	s_ready = true;
	return true;
}

// ---- camera ----

static PlayerInterface* Player()
{
	if (!s_ready || !BenchPlausible(ou))
		return NULL;
	PlayerInterface* pi = ou->player;
	return BenchPlausible(pi) ? pi : NULL;
}

static CameraClass* Camera()
{
	PlayerInterface* pi = Player();
	if (!pi)
		return NULL;
	CameraClass* cam = pi->camera;
	if (!BenchPlausible(cam) || !BenchPlausible(cam->center) || !BenchPlausible(cam->node))
		return NULL;
	return cam;
}

static bool Finite(const float* v, int n)
{
	for (int i = 0; i < n; ++i)
	{
		if (!_finite(v[i]))
			return false;
	}
	return true;
}

bool BenchGetPose(BenchPose* out)
{
	CameraClass* cam = Camera();
	if (!cam || !out)
		return false;
	Ogre::Vector3 c = cam->getCenter();
	float rot[4], node[3];
	s_nodeOrient(cam->center, rot);
	s_nodePosition(cam->node, node);
	BenchPose p;
	p.pos[0] = c.x;
	p.pos[1] = c.y;
	p.pos[2] = c.z;
	for (int i = 0; i < 4; ++i)
		p.rot[i] = rot[i];
	p.zoom = node[2];
	if (!Finite(p.pos, 3) || !Finite(p.rot, 4) || !_finite(p.zoom))
		return false;
	*out = p;
	return true;
}

bool BenchSetPose(const BenchPose& p)
{
	if (!IsMainThread())
		return false;
	CameraClass* cam = Camera();
	float q[4] = { p.rot[0], p.rot[1], p.rot[2], p.rot[3] };
	if (!cam || !Finite(p.pos, 3) || !BenchNormalizeQuat(q))
		return false;
	// Ogre's value constructors are DLL imports we do not link; both types
	// are plain float arrays (w, x, y, z and x, y, z).
	cam->manuallySetOrientationAndZoom(*reinterpret_cast<const Ogre::Quaternion*>(q), BenchClampZoom(p.zoom));
	cam->teleport(*reinterpret_cast<const Ogre::Vector3*>(p.pos));
	return true;
}

static const unsigned MAX_PLAYER_CHARS = 4096;

float BenchNearestPlayerDistance(const float pos[3])
{
	PlayerInterface* pi = Player();
	if (!pi || !Finite(pos, 3))
		return -1.0f;
	unsigned n = GetPlayerCharCount((uintptr_t)pi);
	uintptr_t* chars = GetPlayerCharStuff((uintptr_t)pi);
	if (n > MAX_PLAYER_CHARS || (n && !BenchPlausible(chars)))
		return -1.0f;
	float best = -1.0f;
	for (unsigned i = 0; i < n; ++i)
	{
		uintptr_t c = chars[i];
		if (!BenchPlausible((void*)c))
			continue;
		float cp[3] = { GetCharPosX(c),
		                *(const float*)KLIB_MEMBER(1, c, RootObjectBase_pos_y, OFF_CHAR_POS_Y),
		                GetCharPosZ(c) };
		if (!Finite(cp, 3))
			continue;
		float d = BenchDistance3(pos, cp);
		if (best < 0.0f || d < best)
			best = d;
	}
	return best;
}

bool BenchGetKeyboardCamera()
{
	return s_ready && BenchPlausible(key) && key->controlEnabled;
}

void BenchSetKeyboardCamera(bool enabled)
{
	if (s_ready && IsMainThread() && BenchPlausible(key))
		key->controlEnabled = enabled;
}

// ---- menus and world state ----

static bool OptionsVisible()
{
	OptionsWindow* win = (OptionsWindow*)*s_optionsWindow;   // NULL until first shown
	if (!BenchPlausible(win) || !BenchPlausible(win->mMainWidget))
		return false;
	return win->isVisible();
}

bool BenchMenusClear()
{
	// Both GUI queries construct their window on first use; with a camera
	// the game has already made that first call itself.
	if (!Camera() || !BenchPlausible(gui))
		return false;
	return !gui->isLoading() && !gui->isPaused() && !OptionsVisible();
}

static void* ZoneMgr()
{
	if (!s_ready || !BenchPlausible(ou))
		return NULL;
	void* zm = (void*)ou->zoneMgr;
	return BenchPlausible(zm) ? zm : NULL;
}

bool BenchTransitionClear()
{
	void* zm = ZoneMgr();
	if (!zm || isTransitionActive || transitionEndPending != 0)
		return false;
	unsigned char justLoaded = *(const unsigned char*)KLIB_MEMBER(1, zm, ZoneManager_justLoadedAGame, OFF_ZM_LOADING);
	return GetZoneState(zm) == 0 && justLoaded == 0;
}

bool BenchWorldSettled()
{
	return BenchTransitionClear() && !ou->paused;
}

// ---- camera follow ----

bool BenchFollowAvailable()
{
	return s_ready && s_followReady;
}

bool BenchGetFollowTarget(BenchFollow* out)
{
	PlayerInterface* pi = Player();
	if (!out || !s_followReady || !pi)
		return false;
	memset(out, 0, sizeof(*out));
	memcpy(out->handle, &pi->trackedCharacterHandle, sizeof(hand));
	out->following = pi->trackedCharacterHandle.type != NULL_ITEM;
	return true;
}

bool BenchRestoreFollowTarget(const BenchFollow& f)
{
	PlayerInterface* pi = Player();
	if (!f.following || !s_followReady || !pi || !IsMainThread())
		return false;
	const hand* h = reinterpret_cast<const hand*>(f.handle);
	if (h->type != CHARACTER && h->type != SHOP_TRADER_CLASS)   // what CameraClass::update follows
		return false;
	// startTrackCharacter(NULL) would follow the first selected character instead.
	RootObject* target = h->getRootObject();
	if (!BenchPlausible(target))
		return false;
	pi->startTrackCharacter(target);
	return true;
}

int BenchPlayerCharacterCount()
{
	PlayerInterface* pi = Player();
	if (!pi)
		return -1;
	unsigned n = GetPlayerCharCount((uintptr_t)pi);
	return n > MAX_PLAYER_CHARS ? -1 : (int)n;
}

int BenchLoadedZoneCount()
{
	void* zm = ZoneMgr();
	if (!zm)
		return -1;
	int loaded = 0;
	for (int x = 0; x <= ZONE_GRID_MAX; ++x)
	{
		for (int y = 0; y <= ZONE_GRID_MAX; ++y)
		{
			void* zone = GetZoneEntry(zm, x, y);
			if (zone && (IsZoneLoading(zone) || IsZoneAccessible(zone)))
				++loaded;
		}
	}
	return loaded;
}

// ---- speed ----

float BenchGetSpeed()
{
	return s_ready && BenchPlausible(ou) ? ou->frameSpeedMult : 0.0f;
}

float BenchGetUserNormalSpeed()
{
	float s = BenchGetSpeed();
	if (s > 0.0f)
		return s;
	if (s_ready && (*s_userPauseGuard & 1))
	{
		float saved = *s_userPauseSpeed;
		if (_finite(saved) && saved > 0.0f)
			return saved;
	}
	return 1.0f;
}

static bool SpeedChangeAllowed(float speed)
{
	return s_ready && IsMainThread() && BenchPlausible(ou) && _finite(speed) && speed > 0.0f &&
	       speed <= 1000.0f && BenchMenusClear();
}

bool BenchSetSpeed(float speed)
{
	if (!SpeedChangeAllowed(speed))
		return false;
	ou->setGameSpeed(speed, false);
	return true;
}

bool BenchRestoreSpeed(float speed, bool paused)
{
	if (!(speed > 0.0f))
		speed = 1.0f;
	if (!SpeedChangeAllowed(speed))
		return false;
	ou->setGameSpeed(speed, false);
	// userPause saves the current (non-zero) speed before zeroing it.
	if (paused)
		ou->userPause(true);
	return true;
}

bool BenchPause()
{
	if (!(s_ready && IsMainThread() && BenchPlausible(ou) && BenchMenusClear()))
		return false;
	ou->userPause(true);
	return true;
}

bool BenchSetPausedResumeSpeed(float speed)
{
	if (!s_ready || !IsMainThread() || !_finite(speed) || !(speed > 0.0f) || speed > 1000.0f)
		return false;
	// userPause(false) resumes at the saved speed once the guard bit is set;
	// userPause(true) itself sets the bit on its first save.
	*s_userPauseSpeed = speed;
	*s_userPauseGuard |= 1;
	return true;
}

// ---- window focus ----

bool BenchWindowInForeground()
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

// ---- the clock ----

// The sky object, once its controller is the SkyX one; 0 when unknown.
static uintptr_t Sky(uintptr_t* controller)
{
	if (!s_ready)
		return 0;
	uintptr_t sky = *s_skyInstance;
	if (!BenchPlausible((void*)sky))
		return 0;
	uintptr_t ctl = *(const uintptr_t*)(sky + SKY_CONTROLLER);
	if (!BenchPlausible((void*)ctl) || *(const uintptr_t*)ctl != gameBase + RVA_SKYX_CONTROLLER_VTABLE)
		return 0;
	if (controller)
		*controller = ctl;
	return sky;
}

float BenchGetHour()
{
	uintptr_t ctl = 0;
	if (!Sky(&ctl))
		return -1.0f;
	float h = *(const float*)(ctl + CONTROLLER_HOUR);
	return _finite(h) && h >= 0.0f && h <= 24.0f ? h : -1.0f;
}

int BenchGetDay()
{
	uintptr_t sky = Sky(NULL);
	if (!sky)
		return -1;
	int d = *(const int*)(sky + SKY_DAY);
	return d >= 0 ? d : -1;
}

double BenchGameHoursTotal()
{
	uintptr_t sky = Sky(NULL);
	if (!sky)
		return -1.0;
	double t = *(const double*)(sky + SKY_TOTAL_HOURS);
	return _finite(t) && t >= 0.0 ? t : -1.0;
}

float BenchGetPausedResumeSpeed()
{
	if (!s_ready || !(*s_userPauseGuard & 1))
		return -1.0f;
	float s = *s_userPauseSpeed;
	return _finite(s) ? s : -1.0f;
}

bool BenchClockReady()
{
	return s_ready && s_clockRate != NULL;
}

float BenchClockDefaultRate()
{
	return s_clockDefault;
}

float BenchClockRate()
{
	return BenchClockReady() ? *(volatile const float*)s_clockRate : s_clockDefault;
}

bool BenchSetClockRate(float rate)
{
	if (!BenchClockReady() || !IsMainThread() || !_finite(rate) || rate < 0.0f || rate > 24.0f)
		return false;
	if (*(volatile const float*)s_clockRate == rate)
		return true;
	DWORD old = 0;
	if (!VirtualProtect(s_clockRate, sizeof(float), PAGE_READWRITE, &old))
		return false;
	// One aligned four-byte store; the only reader runs on this thread.
	LONG bits;
	memcpy(&bits, &rate, sizeof(bits));
	InterlockedExchange((volatile LONG*)s_clockRate, bits);
	DWORD ignored = 0;
	VirtualProtect(s_clockRate, sizeof(float), old, &ignored);
	return *(volatile const float*)s_clockRate == rate;
}
