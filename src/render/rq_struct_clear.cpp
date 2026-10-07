// rq_struct_clear.cpp - rqStructClear: Root's remove-render-queue-structures
// flag set while the switch is on, the value read at install restored when it
// goes off (scene_levers.h).
#include "render/scene_levers.h"

#ifdef KEO_DEBUG

#include "render/scene_lever_policy.h"
#include "fixes/fixes_config.h"
#include "base/core.h"
#include <windows.h>
#include <sstream>
#include <string>
#include <string.h>

static const char* const kSetExport = "?setRemoveRenderQueueStructuresOnClear@Root@Ogre@@QEAAX_N@Z";
static const char* const kGetExport = "?getRemoveRenderQueueStructuresOnClear@Root@Ogre@@QEBA_NXZ";
static const char* const kRootExport = "?msSingleton@?$Singleton@VRoot@Ogre@@@Ogre@@1PEAVRoot@2@EA";
static const uintptr_t RVA_SET_REMOVE = 0x43370;
static const uintptr_t RVA_GET_REMOVE = 0x43360;
static const uintptr_t RVA_ROOT_SINGLETON = 0x909B20;

// The setter and getter whole: one byte store or load at Root+0x198, then ret.
static const unsigned char kSetBytes[7] = { 0x88,0x91,0x98,0x01,0x00,0x00,0xC3 };
static const unsigned char kGetBytes[8] = { 0x0F,0xB6,0x81,0x98,0x01,0x00,0x00,0xC3 };

typedef void (*SetRemove_t)(void* root, bool on);
typedef bool (*GetRemove_t)(const void* root);

// Main thread only: the install and the tick are the only users.
static SetRemove_t s_set = NULL;
static GetRemove_t s_get = NULL;
static void* const* s_root = NULL;
static const char* s_install = "not run";
static volatile LONG s_mode = 0;
static int s_seenMode = 0;
static int s_initial = -1;      // the flag read at install or at the first root, -1 before
static int s_applied = 0;       // the mode last written
static int s_rootFlag = -1;     // the flag read back after the last write
static LONG s_sets = 0;
static double s_lastBeat = 0.0;
static const double kBeatSeconds = 60.0;

static bool InstallOk()
{
	return strcmp(s_install, "ok") == 0;
}

static const char* FlagText(int v)
{
	return v < 0 ? "-" : (v ? "1" : "0");
}

void InstallRqStructClear(int* installed, int*)
{
	(void)installed;
	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	const char* why = OgreSceneBuildRefusal(ogre);
	if (!why)
	{
		const uintptr_t base = (uintptr_t)ogre;
		const uintptr_t set = (uintptr_t)GetProcAddress(ogre, kSetExport);
		const uintptr_t get = (uintptr_t)GetProcAddress(ogre, kGetExport);
		const uintptr_t root = (uintptr_t)GetProcAddress(ogre, kRootExport);
		if (set != base + RVA_SET_REMOVE || get != base + RVA_GET_REMOVE || root != base + RVA_ROOT_SINGLETON)
			why = "export mismatch";
		else if (memcmp((const void*)set, kSetBytes, sizeof(kSetBytes)) != 0
		         || memcmp((const void*)get, kGetBytes, sizeof(kGetBytes)) != 0)
			why = "bytes";
		else
		{
			s_set = (SetRemove_t)set;
			s_get = (GetRemove_t)get;
			s_root = (void* const*)root;
		}
	}
	if (why)
	{
		s_install = why;
		LogMsg(std::string("RqClear: install=refused(") + why + ")");
		return;
	}
	s_install = "ok";
	if (*s_root)
		s_initial = s_get(*s_root) ? 1 : 0;
	LogMsg(std::string("RqClear: install=ok initial=") + FlagText(s_initial));
}

static void EmitHeartbeat()
{
	std::ostringstream line;
	line << "RqClear: mode=" << (s_seenMode ? "on" : "off") << " install=";
	if (InstallOk())
		line << "ok";
	else
		line << "refused(" << s_install << ")";
	line << " sets=" << s_sets << " root=" << FlagText(s_rootFlag) << " initial=" << FlagText(s_initial);
	LogMsg(line.str());
}

// Main thread, outside rendering: the next prepareRenderQueue reads the byte.
void RqStructClearTick(double now)
{
	const int mode = (fixes::g_fixesCfg.cfg_rqStructClear == 1 && InstallOk()) ? 1 : 0;
	bool beat = false;
	if (mode != s_seenMode)
	{
		InterlockedExchange(&s_mode, mode);
		s_seenMode = mode;
		beat = true;
	}
	if (InstallOk())
	{
		// A missing root leaves an unapplied change for a later frame.
		void* root = *s_root;
		if (root)
		{
			if (s_initial < 0)
				s_initial = s_get(root) ? 1 : 0;
			const RqClearAction a = RqClearStep(mode, s_applied);
			if (a != RQ_NONE)
			{
				s_set(root, a == RQ_SET_ON ? true : s_initial != 0);
				s_applied = mode == 1 ? 1 : 0;
				++s_sets;
				s_rootFlag = s_get(root) ? 1 : 0;
				beat = true;
			}
		}
	}
	if (beat || now - s_lastBeat >= kBeatSeconds)
	{
		s_lastBeat = now;
		EmitHeartbeat();
	}
}

#else  // !KEO_DEBUG

void InstallRqStructClear(int* installed, int*) { (void)installed; }
void RqStructClearTick(double now) { (void)now; }

#endif // KEO_DEBUG
