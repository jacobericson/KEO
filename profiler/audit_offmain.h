// audit_offmain.h - The off-main probes (OffMainDetail=1): the exe rows and hooks of
// audit_offmain.cpp and the OgreMain rows, worker slots and barrier split of audit_ogre.cpp.

#ifndef KENSHI_FRAME_AUDIT_OFFMAIN_H
#define KENSHI_FRAME_AUDIT_OFFMAIN_H

#include "audit_detail.h"
#include "audit_sync_split.h"

namespace kenshiframeaudit_detail {

// Raises *p to v; any thread.
inline void AtomicMax64(volatile LONG64* p, LONG64 v)
{
	LONG64 cur = *p;
	while (v > cur)
	{
		LONG64 prev = InterlockedCompareExchange64(p, v, cur);
		if (prev == cur)
			return;
		cur = prev;
	}
}

// The main thread's Barrier::sync call in flight (hk_BarrierSync).
struct OgreSyncCall
{
	int      bucket;    // syncsplit::RequestBucket of SceneManager+0x4B18
	int      kind;      // syncsplit::SyncKind
	bool     blocked;   // the call will wait for another thread
	LONGLONG t0;
};

void InstallOffMain(bool steam);          // startup, after InstallCursor (main thread)
const char* OffMainStatus();              // "on", "partial" or "off"
// OnProbeEnter / OnProbeExit hand every tag at or above ST_OFF_FIRST here first.
void OffMainProbeEnter(int tag, CallSiteProbe::U64 a, CallSiteProbe::U64 b, CallSiteProbe::U64 c, CallSiteProbe::U64 d);
void OffMainProbeExit(int tag, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1);
void OffMainParticlesEnter();             // main: the particles row, after its census
void OffMainIndoorsOther(LONGLONG ticks, bool physicsRunning);   // main: isIndoors outside mouseScan
void OffMainFrameTotals(FrameRec& r);     // main, at frame close
void OffMainOncePerSecond();              // main: the [AUDIT-SYNC] line every 5 s
void OgreSyncEnter(void* barrier, OgreSyncCall* c);   // main: hk_BarrierSync
void OgreSyncExit(const OgreSyncCall& c);
void OgreOldAnimsEnter();                 // main: hk_OldAnims, around the original
void OgreOldAnimsExit();
// "on" when all `total` parts installed, "partial" when some did, "off" when none.
inline const char* OffMainPartWord(int n, int total)
{
	return n >= total ? "on" : (n > 0 ? "partial" : "off");
}

// audit_offmain.cpp, used by audit_ogre.cpp (main thread, at install):
// Installs rows[idx[0..n)] into `module` in one Install call, every row DisableSites names left out
// with a `SKIP ini` line, and writes each other row's site line; returns the number installed.
int InstallOffMainRows(HMODULE module, CallSiteProbe::Site* rows, const int* idx, int n);
// Marks each tag of rows[0..count) present only when every row carrying it installed.
void MarkOffMainTags(const CallSiteProbe::Site* rows, int count);

// audit_ogre.cpp, used by audit_offmain.cpp:
const int NUM_OGRE_ROWS = 10;
int InstallOgreProbes(HMODULE ogre);      // the rows installed, 0 when the Ogre part is refused
void OgreProbeEnter(int tag, CallSiteProbe::U64 a, CallSiteProbe::U64 b);
void OgreProbeExit(int tag, LONGLONG t0, LONGLONG t1);
void OgreFrameTotals(FrameRec& r);

} // kenshiframeaudit_detail

#endif
