// ogre_worker_priority.cpp - The Ogre worker priority: its install step and the main-thread tick
// that, on a switch, raises the main scene manager's worker threads to above-normal priority or
// puts back what they had, and writes the OgrePriority: line. A raise that finds no main scene
// manager is tried again each second while the switch stays on. The threads are found through the
// scene manager's own handle vector, so no other scene manager's worker is ever touched.
#include "render/ogre_worker_priority.h"

#ifdef KEO_DEBUG

#include "render/ogre_worker_policy.h"
#include "render/module_hooks.h"
#include "base/core.h"
#include "game/game.h"
#include "fixes/fixes_config.h"
#include <windows.h>
#include <sstream>
#include <string>

static_assert(OGRE_PRIORITY_RAISED == THREAD_PRIORITY_ABOVE_NORMAL, "the raised priority is above normal");
static_assert(OGRE_PRIORITY_ERROR == THREAD_PRIORITY_ERROR_RETURN, "the priority error value");

typedef LONG (WINAPI* NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// NtQueryInformationThread's class that answers the address the thread was started at.
static const ULONG THREAD_QUERY_WIN32_START_ADDRESS = 9;

namespace ogre_worker_priority_detail {
struct SavedPriority
{
	HANDLE h;
	DWORD  tid;
	int    prio;
};
} // namespace ogre_worker_priority_detail
using namespace ogre_worker_priority_detail;

// Main thread only: the tick is the only code that reads or writes them.
static NtQueryInformationThread_t s_ntQuery = NULL;
static uintptr_t     s_ogreBase = 0;
static SavedPriority s_saved[OGRE_MAX_WORKERS];
static int           s_savedCount = 0;
static LONG          s_raised = 0;
static LONG          s_restored = 0;
static LONG          s_foreign = 0;
static LONG          s_failed = 0;
static int           s_workers = 0;
static bool          s_sceneMissing = false;
static double        s_lastTry = 0;
static int           s_seenMode = 0;
static const char*   s_installWhy = "not run";
static double        s_lastBeat = 0;

static uintptr_t MainSceneManager()
{
	void* holder = GameAddr(RVA_RENDERER);
	if (!holder)
		return 0;
	const uintptr_t renderer = *(const uintptr_t*)holder;
	if (!renderer)
		return 0;
	return *(const uintptr_t*)KLIB_MEMBER(5, renderer, Renderer_scene, 0x60);
}

// One worker: its handle confirmed by its start routine, its priority saved, then raised.
static void RaiseOne(uintptr_t slot, uintptr_t start)
{
	const void* th = *(void* const*)slot;
	HANDLE h = th ? *(const HANDLE*)th : NULL;
	if (!h)
	{
		++s_failed;
		return;
	}
	void* startAddr = NULL;
	if (s_ntQuery(h, THREAD_QUERY_WIN32_START_ADDRESS, &startAddr, sizeof(startAddr), NULL) < 0)
	{
		++s_failed;
		return;
	}
	if ((uintptr_t)startAddr != start)
	{
		++s_foreign;
		return;
	}
	const DWORD tid = GetThreadId(h);
	const int prio = GetThreadPriority(h);
	if (!tid || !OgrePriorityRestorable(prio) || !SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL))
	{
		++s_failed;
		return;
	}
	SavedPriority& s = s_saved[s_savedCount++];
	s.h = h;
	s.tid = tid;
	s.prio = prio;
	++s_raised;
}

static void Raise()
{
	if (s_savedCount > 0)
		return;
	const uintptr_t sm = MainSceneManager();
	s_sceneMissing = !sm;
	if (!sm)
	{
		s_workers = 0;
		return;
	}
	const unsigned long long n = *(const unsigned long long*)(sm + OGRE_SM_WORKER_COUNT);
	const uintptr_t begin = *(const uintptr_t*)(sm + OGRE_SM_WORKERS_BEGIN);
	const uintptr_t end = *(const uintptr_t*)(sm + OGRE_SM_WORKERS_END);
	const int slots = OgreWorkerSlots(begin, end);
	s_workers = slots < 0 ? 0 : slots;
	if (!OgreWorkersUsable(slots, n))
	{
		++s_failed;
		return;
	}
	const uintptr_t start = s_ogreBase + OGRE_WORKER_START_RVA;
	for (int i = 0; i < slots; ++i)
		RaiseOne(begin + (uintptr_t)i * OGRE_WORKER_SLOT_SIZE, start);
}

// A handle whose thread id changed was closed or reused: it is left alone.
static void Restore()
{
	for (int i = 0; i < s_savedCount; ++i)
	{
		const SavedPriority& s = s_saved[i];
		if (GetThreadId(s.h) == s.tid && SetThreadPriority(s.h, s.prio))
			++s_restored;
		else
			++s_failed;
	}
	s_savedCount = 0;
}

static const char* OgrePriorityInstall()
{
	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	if (!ogre)
		return "module";
	DWORD stamp = 0, size = 0;
	if (!ReadModuleImageId(ogre, &stamp, &size) || !OgreMainIdentityOk(stamp, size))
		return "build";
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	s_ntQuery = ntdll ? (NtQueryInformationThread_t)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
	if (!s_ntQuery)
		return "ntdll";
	s_ogreBase = (uintptr_t)ogre;
	return NULL;
}

void InstallOgreWorkerPriority(int* installed, int*)
{
	(void)installed;
	const char* why = OgrePriorityInstall();
	s_installWhy = why;
	if (why)
		ErrorLog(std::string("OgrePriority: install=refused(") + why + ")");
	else
		LogMsg("OgrePriority: install=ok");
}

static void OgrePriorityHeartbeat(double now)
{
	std::ostringstream ss;
	ss << "OgrePriority: mode=" << (s_seenMode ? "on" : "off");
	if (s_installWhy)
		ss << " install=refused(" << s_installWhy << ")";
	else
		ss << " install=ok";
	ss << " workers=" << s_workers << " raised=" << s_raised << " restored=" << s_restored
	   << " foreign=" << s_foreign << " failed=";
	if (s_sceneMissing)
		ss << "scene";
	else
		ss << s_failed;
	LogMsg(ss.str());
	s_lastBeat = now;
}

void OgreWorkerPriorityTick(double now)
{
	const int mode = fixes::g_fixesCfg.cfg_ogreWorkerPriority ? 1 : 0;
	if (mode != s_seenMode)
	{
		if (!s_installWhy)
		{
			if (mode)
			{
				Raise();
				s_lastTry = now;
			}
			else
			{
				Restore();
				s_sceneMissing = false;
			}
		}
		s_seenMode = mode;
		OgrePriorityHeartbeat(now);
		return;
	}
	// A switch turned on before the main scene manager existed raises once it does.
	if (!s_installWhy && OgrePriorityRetryDue(mode != 0, s_sceneMissing, now, s_lastTry))
	{
		Raise();
		s_lastTry = now;
	}
	if (now - s_lastBeat >= 60.0)
		OgrePriorityHeartbeat(now);
}

#else  // !KEO_DEBUG

void InstallOgreWorkerPriority(int* installed, int*) { (void)installed; }
void OgreWorkerPriorityTick(double now) { (void)now; }

#endif // KEO_DEBUG
