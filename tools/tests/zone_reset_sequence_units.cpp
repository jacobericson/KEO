#include <cstdio>
#include <string>
#include "zone/reset/zone_reset_sequence.h"

#include "check.h"

// Recording operations. Each call appends one letter to `trace`:
// D drainBegin, d drainEnd, L lock, U unlock, N nativeUnload, C collectSurvivors,
// Q survivorClaimed, X unloadSurvivor, K keepSurvivor.
struct Fake
{
	bool          drained;
	unsigned      drainWaited;
	ZoneResetLock lockResult;
	unsigned      lockWaited;
	int           survivors;
	bool          claimed[8];
	bool          throwInNative;

	std::string   trace;
	unsigned      drainTimeoutAsked;
	unsigned      lockTimeoutAsked;
	int           unloaded;
	int           keptCalls;
};

static void InitFake(Fake* f)
{
	f->drained       = true;
	f->drainWaited   = 0;
	f->lockResult    = ZONE_RESET_LOCK_HELD;
	f->lockWaited    = 0;
	f->survivors     = 0;
	for (int i = 0; i < 8; ++i)
		f->claimed[i] = false;
	f->throwInNative = false;
	f->trace.clear();
	f->drainTimeoutAsked = 0;
	f->lockTimeoutAsked  = 0;
	f->unloaded  = 0;
	f->keptCalls = 0;
}

static Fake* F(void* ctx) { return (Fake*)ctx; }

static bool FakeDrainBegin(void* ctx, unsigned timeoutMs, unsigned* waitedMs)
{
	F(ctx)->trace += 'D';
	F(ctx)->drainTimeoutAsked = timeoutMs;
	*waitedMs = F(ctx)->drainWaited;
	return F(ctx)->drained;
}

static void FakeDrainEnd(void* ctx) { F(ctx)->trace += 'd'; }

static ZoneResetLock FakeLock(void* ctx, unsigned timeoutMs, unsigned* waitedMs)
{
	F(ctx)->trace += 'L';
	F(ctx)->lockTimeoutAsked = timeoutMs;
	*waitedMs = F(ctx)->lockWaited;
	return F(ctx)->lockResult;
}

static void FakeUnlock(void* ctx) { F(ctx)->trace += 'U'; }

static void FakeNativeUnload(void* ctx)
{
	F(ctx)->trace += 'N';
	if (F(ctx)->throwInNative)
		throw 7;
}

static int FakeCollect(void* ctx)
{
	F(ctx)->trace += 'C';
	return F(ctx)->survivors;
}

static bool FakeClaimed(void* ctx, int i)
{
	F(ctx)->trace += 'Q';
	return F(ctx)->claimed[i];
}

static void FakeUnloadSurvivor(void* ctx, int)
{
	F(ctx)->trace += 'X';
	F(ctx)->unloaded++;
}

static void FakeKeepSurvivor(void* ctx, int)
{
	F(ctx)->trace += 'K';
	F(ctx)->keptCalls++;
}

static ZoneResetFenceOps MakeOps(Fake* f)
{
	ZoneResetFenceOps ops;
	ops.ctx              = f;
	ops.drainBegin       = &FakeDrainBegin;
	ops.drainEnd         = &FakeDrainEnd;
	ops.lock             = &FakeLock;
	ops.unlock           = &FakeUnlock;
	ops.nativeUnload     = &FakeNativeUnload;
	ops.collectSurvivors = &FakeCollect;
	ops.survivorClaimed  = &FakeClaimed;
	ops.unloadSurvivor   = &FakeUnloadSurvivor;
	ops.keepSurvivor     = &FakeKeepSurvivor;
	return ops;
}

static ZoneResetFenceOutcome Run(Fake* f, bool unloadSurvivors)
{
	ZoneResetFenceOps ops = MakeOps(f);
	ZoneResetFenceOutcome out;
	out.drained = false;
	out.drainMs = 0;
	out.lock = ZONE_RESET_LOCK_NONE;
	out.lockWaitMs = 0;
	out.fenceComplete = false;
	out.survivors = -1;
	out.kept = -1;
	ZoneResetRunFence(&ops, 10000, 500, unloadSurvivors, &out);
	return out;
}

static bool Has(const std::string& s, char c) { return s.find(c) != std::string::npos; }

int main()
{
	Fake f;

	InitFake(&f);
	f.survivors = 2;
	Run(&f, true);
	Check(f.trace == "DLNCXXUd",
	      "sequence: drain, lock, native unload, survivors, release, drain end, in that order");

	InitFake(&f);
	f.drainWaited = 2500;
	Run(&f, true);
	Check(f.drainTimeoutAsked == 10000 && f.lockTimeoutAsked == 7500,
	      "sequence: the lock's budget is what the drain left");

	InitFake(&f);
	f.drained = false;
	f.drainWaited = 10000;
	Run(&f, true);
	Check(f.lockTimeoutAsked == 500,
	      "sequence: a drain that used the whole budget leaves the lock the floor");

	InitFake(&f);
	f.lockResult = ZONE_RESET_LOCK_TIMEOUT;
	f.lockWaited = 10000;
	{
		ZoneResetFenceOutcome out = Run(&f, true);
		Check(Has(f.trace, 'N') && out.lock == ZONE_RESET_LOCK_TIMEOUT && out.lockWaitMs == 10000
		      && !out.fenceComplete,
		      "sequence: the native unload runs when the lock timed out");
	}

	InitFake(&f);
	f.drained = false;
	f.drainWaited = 10000;
	{
		ZoneResetFenceOutcome out = Run(&f, true);
		Check(Has(f.trace, 'N') && !out.drained && out.drainMs == 10000 && !out.fenceComplete,
		      "sequence: the native unload runs when the drain timed out");
	}

	{
		bool ok = true;
		InitFake(&f);
		f.lockResult = ZONE_RESET_LOCK_TIMEOUT;
		Run(&f, true);
		ok = ok && f.trace == "DLNCd";
		InitFake(&f);
		f.lockResult = ZONE_RESET_LOCK_NONE;
		Run(&f, true);
		ok = ok && f.trace == "DLNCd";
		Check(ok, "sequence: nothing is released that was not held");
	}

	InitFake(&f);
	f.survivors = 3;
	f.claimed[0] = f.claimed[1] = f.claimed[2] = true;
	{
		ZoneResetFenceOutcome out = Run(&f, true);
		Check(out.fenceComplete && !Has(f.trace, 'Q') && f.unloaded == 3 && f.keptCalls == 0
		      && out.survivors == 3 && out.kept == 0,
		      "sequence: with the fence complete no claim is asked for and every survivor is unloaded");
	}

	InitFake(&f);
	f.lockResult = ZONE_RESET_LOCK_TIMEOUT;
	f.survivors = 2;
	f.claimed[0] = true;
	{
		ZoneResetFenceOutcome out = Run(&f, true);
		Check(!out.fenceComplete && f.trace == "DLNCQKQXd" && out.kept == 1 && out.survivors == 2,
		      "sequence: with the fence incomplete a claimed survivor is kept and an unclaimed one unloaded");
	}

	InitFake(&f);
	f.lockResult = ZONE_RESET_LOCK_TIMEOUT;
	f.survivors = 3;
	f.claimed[0] = true;
	{
		ZoneResetFenceOutcome out = Run(&f, false);
		Check(out.survivors == 3 && out.kept == 0 && f.trace == "DLNCd",
		      "sequence: with the unload off every survivor is counted and left, none asked about");
	}

	InitFake(&f);
	f.survivors = 2;
	f.throwInNative = true;
	{
		bool caught = false;
		try
		{
			Run(&f, true);
		}
		catch (int v)
		{
			caught = (v == 7);
		}
		Check(caught && f.trace == "DLNUd",
		      "sequence: an unwind out of the native unload still releases the lock, then ends the drain");
	}

	return CheckExit("zone_reset_sequence_units");
}
