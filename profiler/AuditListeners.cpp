// Per-class frame-listener timer. Every 5 s, on the main thread, walks
// Ogre::Root's frame-listener set, points each new listener class's four
// callback slots at timing thunks, and prints each class's mean ms per frame
// on [AUDIT-LISTENERS]. Root fires frameStarted / Queued / Ended from
// renderOneFrame, and the post-animation callback from
// SceneManager::updateAllOldAnimations inside the main thread's cull, never
// from its animation workers; so the per-class counters are plain adds.

#include "KenshiFrameAudit_internal.h"
#include <set>
#include <cstring>

#pragma warning(push)
#pragma warning(disable: 4482 4005 4099)
#include <OgrePrerequisites.h>
#pragma warning(pop)

using namespace audit;

namespace auditlisteners_detail
{

typedef bool (*FrameListenerFn)(void* self, const void* evt);
typedef void (*PostAnimFn)(void* self);
typedef void* (*RootSingleton_t)();

// Root's listener sets use Ogre's allocator, which makes them 40 bytes, not
// the 32 of a std::set with the default allocator.
typedef Ogre::set<void*>::type ListenerSet;
typedef char ListenerSetSizeCheck[sizeof(ListenerSet) == KLIB_SIZE_Root_listenerSet ? 1 : -1];

// Kenshi's OgreMain fork calls frameStarted, frameRenderingQueued and
// frameEnded through vtable slots 0, 1 and 3. Slot 2 is its no-argument
// post-animation callback (Root::_fireFrameListenerPostAnimationThreads).
// Indexes below: 0 started, 1 queued, 2 ended, 3 post-animation.
const int NUM_CB     = 4;
const int SLOT_OF[NUM_CB] = { 0, 1, 3, 2 };
const int SLOT_SPAN  = 4;

struct ListenerClass
{
	void**   vtable;
	void*    orig[NUM_CB];
	LONGLONG ticks[NUM_CB];
	LONG     calls[NUM_CB];
	char     name[96];
};

const int MAX_CLASSES = 48;
ListenerClass g_classes[MAX_CLASSES];
int  g_classCount = 0;
bool g_enabled = false;
LONGLONG g_lastReport = 0;
LONG g_frames = 0;
RootSingleton_t s_rootSingleton = NULL;

bool Call(int c, int slot, void* self, const void* evt)
{
	ListenerClass& k = g_classes[c];
	LONGLONG t0 = Now();
	bool r = ((FrameListenerFn)k.orig[slot])(self, evt);
	k.ticks[slot] += Now() - t0;
	k.calls[slot]++;
	return r;
}

void CallPost(int c, void* self)
{
	ListenerClass& k = g_classes[c];
	LONGLONG t0 = Now();
	((PostAnimFn)k.orig[3])(self);
	k.ticks[3] += Now() - t0;
	k.calls[3]++;
}

// One distinct function per class and callback: build.bat links with
// /OPT:NOICF, so identical bodies are not folded.
template <int I> struct Thunk
{
	static bool Started(void* s, const void* e) { return Call(I, 0, s, e); }
	static bool Queued(void* s, const void* e)  { return Call(I, 1, s, e); }
	static bool Ended(void* s, const void* e)   { return Call(I, 2, s, e); }
	static void Post(void* s)                   { CallPost(I, s); }
};

struct ThunkSet { void* fn[NUM_CB]; };
template <int N> struct ThunkTable
{
	static void Fill(ThunkSet* t)
	{
		ThunkTable<N - 1>::Fill(t);
		t[N - 1].fn[0] = (void*)&Thunk<N - 1>::Started;
		t[N - 1].fn[1] = (void*)&Thunk<N - 1>::Queued;
		t[N - 1].fn[2] = (void*)&Thunk<N - 1>::Ended;
		t[N - 1].fn[3] = (void*)&Thunk<N - 1>::Post;
	}
};
template <> struct ThunkTable<0> { static void Fill(ThunkSet*) {} };
ThunkSet g_thunks[MAX_CLASSES];

// MSVC x64 RTTI: vtable[-1] is the complete-object locator (signature 1); its
// type descriptor is image-relative at +0xC, and the name starts 16 bytes in.
// Without readable RTTI the class is named by module and vtable offset.
void ClassName(void** vtable, uintptr_t mb, uintptr_t me, char* out, size_t cap)
{
	out[0] = 0;
	uintptr_t col = (uintptr_t)vtable[-1];
	if (PlausiblePtr(col) && col >= mb && col + 0x18 <= me && *(const unsigned*)col == 1)
	{
		uintptr_t name = mb + *(const unsigned*)(col + 0xC) + 16;
		if (name + 4 < me && strncmp((const char*)name, ".?A", 3) == 0 &&
		    memchr((const void*)name, 0, me - name) != NULL)
		{
			strncpy_s(out, cap, (const char*)name, _TRUNCATE);
			return;
		}
	}
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA((HMODULE)mb, path, MAX_PATH);
	const char* file = "";
	if (n > 0 && n < MAX_PATH)
	{
		const char* slash = strrchr(path, '\\');
		file = slash ? slash + 1 : path;
	}
	_snprintf_s(out, cap, _TRUNCATE, "?%s+0x%IX", file, (uintptr_t)vtable - mb);
}

bool Patch(void** vtable, int c)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(vtable, &mbi, sizeof(mbi)) != sizeof(mbi))
		return false;
	bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
	                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	DWORD writable = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;

	DWORD old = 0;
	if (!VirtualProtect(vtable, SLOT_SPAN * sizeof(void*), writable, &old))
		return false;
	for (int s = 0; s < NUM_CB; ++s)
	{
		g_classes[c].orig[s] = vtable[SLOT_OF[s]];
		vtable[SLOT_OF[s]] = g_thunks[c].fn[s];
	}
	DWORD ignored = 0;
	VirtualProtect(vtable, SLOT_SPAN * sizeof(void*), old, &ignored);
	return true;
}

void Scan()
{
	char* root = s_rootSingleton ? (char*)s_rootSingleton() : NULL;
	if (!root)
		return;
	const ListenerSet* listeners = (const ListenerSet*)(root + KLIB_OFF_Root_frameListeners);
	const ListenerSet* removed   = (const ListenerSet*)(root + KLIB_OFF_Root_removedFrameListeners);
	for (ListenerSet::const_iterator it = listeners->begin(); it != listeners->end(); ++it)
	{
		// A listener removed this frame stays in the set until Root's next
		// sync and may already be deleted.
		if (!PlausiblePtr((uintptr_t)*it) || removed->find(*it) != removed->end())
			continue;
		void** vtable = *(void***)(*it);
		uintptr_t mb = 0, me = 0;
		if (!PlausiblePtr((uintptr_t)vtable) || !ModuleRange(vtable, &mb, &me) ||
		    (uintptr_t)vtable - sizeof(void*) < mb || (uintptr_t)(vtable + SLOT_SPAN) > me)
			continue;
		bool known = false;
		for (int c = 0; c < g_classCount; ++c)
			if (g_classes[c].vtable == vtable) { known = true; break; }
		if (known || g_classCount >= MAX_CLASSES)
			continue;
		ListenerClass& k = g_classes[g_classCount];
		memset(&k, 0, sizeof(k));
		k.vtable = vtable;
		ClassName(vtable, mb, me, k.name, sizeof(k.name));
		if (Patch(vtable, g_classCount))
			++g_classCount;
	}
}

void Report()
{
	LONG frames = InterlockedExchange(&g_frames, 0);
	if (frames <= 0 || g_classCount == 0)
		return;
	// Root fires the post-animation callback on some culls only; every class
	// is called each time, so the least-called class counts the fires.
	LONG postFires = g_classes[0].calls[3];
	for (int c = 1; c < g_classCount; ++c)
		if (g_classes[c].calls[3] < postFires)
			postFires = g_classes[c].calls[3];
	std::string line = Fmt("[AUDIT-LISTENERS] classes=%d frames=%ld postPerFrame=%.2f",
	                       g_classCount, frames, (double)postFires / frames);
	for (int c = 0; c < g_classCount; ++c)
	{
		ListenerClass& k = g_classes[c];
		line += Fmt(" | %s s=%.3f q=%.3f e=%.3f p=%.3f", k.name,
		            TicksToMs(k.ticks[0]) / frames, TicksToMs(k.ticks[1]) / frames,
		            TicksToMs(k.ticks[2]) / frames, TicksToMs(k.ticks[3]) / frames);
		for (int i = 0; i < NUM_CB; ++i)
		{
			k.ticks[i] = 0;
			k.calls[i] = 0;
		}
	}
	AuditLine(line);
}

} // namespace
using namespace auditlisteners_detail;

bool Listeners_Init(bool enabled)
{
	ThunkTable<MAX_CLASSES>::Fill(g_thunks);
	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	s_rootSingleton = ogre ? (RootSingleton_t)GetProcAddress(ogre, "?getSingletonPtr@Root@Ogre@@SAPEAV12@XZ") : NULL;
	g_enabled = enabled && s_rootSingleton != NULL;
	return g_enabled;
}

void Listeners_OnFrameStarted()
{
	if (!g_enabled)
		return;
	InterlockedIncrement(&g_frames);
	LONGLONG now = Now();
	if (now - g_lastReport < g_qpcFreq * 5)
		return;
	g_lastReport = now;
	// Report first so a window covers only classes timed for all of it.
	Report();
	Scan();
}
