#include "zone/geometry/zone_geometry_epoch.h"

#if ZONEHAND_STEP >= 1

#include "base/core.h"

#include <sstream>

// epoch in the low 32 bits, in-flight depth in the high 32. Both halves move
// in a single compare-exchange, so a reader never sees a lowered depth
// without the raised epoch that goes with it, and an epoch that wraps cannot
// carry into the depth.
// Main mod-geometry mutations publish both halves by one 64-bit CAS;
// generation and cache-store readers on bg/workers take one atomic snapshot.
// This certificate input cannot tear into halves. A world reset advances
// the epoch rather than zeroing it; the 32-bit epoch can wrap.
static volatile LONGLONG g_geomWord = 0;

static const LONGLONG GEOM_EPOCH_MASK = 0x00000000FFFFFFFFLL;

static LONGLONG GeomRead()
{
	return InterlockedCompareExchange64((volatile LONGLONG*)&g_geomWord, 0, 0);
}

ZoneGeometrySnapshot ZoneGeometrySnapshotNow()
{
	LONGLONG w = GeomRead();
	ZoneGeometrySnapshot s;
	s.epoch         = (unsigned)(w & GEOM_EPOCH_MASK);
	s.mutationDepth = (unsigned)((ULONGLONG)w >> 32);
	return s;
}

// depthDelta is -1, 0 or +1; epochDelta 0 or +1. The two halves are updated
// together so no reader can observe one without the other.
static void GeomUpdate(int depthDelta, unsigned epochDelta)
{
	for (;;)
	{
		LONGLONG cur = GeomRead();
		unsigned epoch = (unsigned)(cur & GEOM_EPOCH_MASK);
		unsigned depth = (unsigned)((ULONGLONG)cur >> 32);
		if (depthDelta < 0)
		{
			if (depth == 0)
				return;      // unpaired end; leave the word alone
			--depth;
		}
		else if (depthDelta > 0)
		{
			++depth;
		}
		epoch += epochDelta;
		LONGLONG next = (LONGLONG)(((ULONGLONG)depth << 32) | (ULONGLONG)epoch);
		if (InterlockedCompareExchange64((volatile LONGLONG*)&g_geomWord, next, cur) == cur)
			return;
	}
}

void ZoneGeometryMutationBegin() { GeomUpdate(+1, 0); }
void ZoneGeometryMutationEnd()   { GeomUpdate(-1, 1); }
void ZoneGeometryNoteBoundary()  { GeomUpdate(0, 1); }

ZoneGeometryMode ZoneGeometryActiveMode()
{
	return zone::g_zoneCfg.zoneGeometryMode;
}

// Per-thread certificate. POD, so the thread-local initializer is a constant
// and no runtime construction happens on a Havok thread.
static __declspec(thread) unsigned t_certEpoch = 0;
static __declspec(thread) int      t_certCellX = -1;
static __declspec(thread) int      t_certCellY = -1;
static __declspec(thread) int      t_certValid = 0;

static volatile long g_certCaptured   = 0;
static volatile long g_certChecked    = 0;
static volatile long g_certCurrent    = 0;
static volatile long g_certNoCapture  = 0;
static volatile long g_certWrongCell  = 0;
static volatile long g_certCapRaced   = 0;
static volatile long g_certEpochMoved = 0;
static volatile long g_certStoreRaced = 0;
static volatile long g_certRefused    = 0;   // verdicts actually acted on

void ZoneGeometryCaptureForJob(int gridX, int gridY)
{
	if (gridX < 0 || gridY < 0)
	{
		// No cell to certify: leave the "none" certificate, which the check
		// reports as noCapture rather than silently passing.
		t_certCellX = -1;
		t_certCellY = -1;
		t_certValid = 0;
		return;
	}
	ZoneGeometrySnapshot at = ZoneGeometrySnapshotNow();
	ZoneGeometryCertificate cert = ZoneGeometryCertificateCapture(at, gridX, gridY);
	t_certEpoch = cert.capturedEpoch;
	t_certCellX = cert.cellX;
	t_certCellY = cert.cellY;
	t_certValid = cert.valid ? 1 : 0;
	InterlockedIncrement(&g_certCaptured);
}

bool ZoneGeometryStoreRefused(int gridX, int gridY)
{
	ZoneGeometryCertificate cert;
	cert.capturedEpoch = t_certEpoch;
	cert.cellX         = t_certCellX;
	cert.cellY         = t_certCellY;
	cert.valid         = (t_certValid != 0);

	ZoneGeometrySnapshot atStore = ZoneGeometrySnapshotNow();
	ZoneCertStaleness verdict = ZoneGeometryCertificateCheck(cert, atStore, gridX, gridY);

	InterlockedIncrement(&g_certChecked);
	switch (verdict)
	{
	case ZONE_CERT_CURRENT:       InterlockedIncrement(&g_certCurrent);    break;
	case ZONE_CERT_NO_CAPTURE:    InterlockedIncrement(&g_certNoCapture);  break;
	case ZONE_CERT_WRONG_CELL:    InterlockedIncrement(&g_certWrongCell);  break;
	case ZONE_CERT_CAPTURE_RACED: InterlockedIncrement(&g_certCapRaced);   break;
	case ZONE_CERT_EPOCH_MOVED:   InterlockedIncrement(&g_certEpochMoved); break;
	case ZONE_CERT_STORE_RACED:   InterlockedIncrement(&g_certStoreRaced); break;
	}

	// The certificate is spent: a second store on this thread without a
	// fresh capture must read noCapture, not the previous job's answer.
	t_certCellX = -1;
	t_certCellY = -1;
	t_certValid = 0;

	bool refused = ZoneGeometryRejectsStore(ZoneGeometryActiveMode(), verdict);
	if (refused)
		InterlockedIncrement(&g_certRefused);
	return refused;
}

void ZoneGeometryCertReset()
{
	InterlockedExchange(&g_certCaptured,   0);
	InterlockedExchange(&g_certChecked,    0);
	InterlockedExchange(&g_certCurrent,    0);
	InterlockedExchange(&g_certNoCapture,  0);
	InterlockedExchange(&g_certWrongCell,  0);
	InterlockedExchange(&g_certCapRaced,   0);
	InterlockedExchange(&g_certEpochMoved, 0);
	InterlockedExchange(&g_certStoreRaced, 0);
	InterlockedExchange(&g_certRefused,    0);
}

void ZoneGeometryCertTick(double now)
{
	// Unconditional: a line that stopped printing when the counts were all
	// zero would be indistinguishable from a capture site that never ran,
	// which is the one reading this measurement exists to tell apart.
	static double nextReport = 0.0;
	if (nextReport == 0.0)
		nextReport = now + 60.0;
	if (now < nextReport)
		return;
	nextReport = now + 60.0;

	ZoneGeometrySnapshot s = ZoneGeometrySnapshotNow();
	std::ostringstream ss;
	ss << "ZoneGeomCert: epoch=" << s.epoch << " inFlight=" << s.mutationDepth
	   << " cap=" << InterlockedCompareExchange(&g_certCaptured, 0, 0)
	   << " chk=" << InterlockedCompareExchange(&g_certChecked, 0, 0)
	   << " cur=" << InterlockedCompareExchange(&g_certCurrent, 0, 0)
	   << " noCap=" << InterlockedCompareExchange(&g_certNoCapture, 0, 0)
	   << " cell=" << InterlockedCompareExchange(&g_certWrongCell, 0, 0)
	   << " capRace=" << InterlockedCompareExchange(&g_certCapRaced, 0, 0)
	   << " moved=" << InterlockedCompareExchange(&g_certEpochMoved, 0, 0)
	   << " storeRace=" << InterlockedCompareExchange(&g_certStoreRaced, 0, 0)
	   << " refused=" << InterlockedCompareExchange(&g_certRefused, 0, 0)
	   << " mode=" << (ZoneGeometryActiveMode() == ZONE_GEOMETRY_CONTENT_ONLY
	                   ? "contentOnly" : "lateAdopt");
	LogMsg(ss.str());
}

#endif // ZONEHAND_STEP >= 1
