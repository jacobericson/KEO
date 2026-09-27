// Host tests for the build-lock stall bound (src/navmesh/nm_buildlock_stall.{h,cpp}).
//
// The replays below are the measured shapes: across fifty sessions of healthy
// play the longest unbroken run of refusals lasted two seconds and the busiest
// second carried 230 of them; the session this bound exists for ran 75 seconds
// unbroken at roughly 15,700 a second.
#include "navmesh/jobs/nm_buildlock_stall.h"

#include <cstdio>

#include "check.h"


// The run bookkeeping the hook does, so the replays exercise the decision the
// way it is actually reached.
struct Run
{
	long   length;
	double startedAt;
	bool   latched;
	long   latches;
	long   throttles;
	Run() : length(0), startedAt(0.0), latched(false), latches(0), throttles(0) {}
};

// `decide` is a parameter so a deliberately broken rule can be run through the
// same replays; a test that cannot fail proves nothing about the one that can.
typedef BuildLockStallAction (*DecideFn)(const BuildLockStallInputs&);

static void Step(Run& r, bool granted, double now, DecideFn decide)
{
	BuildLockStallInputs in;
	in.granted    = granted;
	in.runLength  = 0;
	in.runSeconds = 0.0;
	in.latched    = r.latched;
	if (!granted)
	{
		if (r.length == 0)
			r.startedAt = now;
		r.length++;
		in.runLength  = r.length;
		in.runSeconds = now - r.startedAt;
	}

	switch (decide(in))
	{
	case BL_STALL_RESET:
		r.length = 0; r.startedAt = 0.0; r.latched = false;
		break;
	case BL_STALL_LATCH:
		r.latched = true; r.latches++; r.throttles++;
		break;
	case BL_STALL_THROTTLE:
		r.throttles++;
		break;
	case BL_STALL_PASS:
		break;
	}
}

// A burst of `perSecond` refusals a second for `seconds`, then one grant.
static Run Replay(double perSecond, double seconds, DecideFn decide)
{
	Run r;
	double step = 1.0 / perSecond;
	for (double t = 0.0; t < seconds; t += step)
		Step(r, false, t, decide);
	Step(r, true, seconds, decide);
	return r;
}

// The deliberate break: the same rule with both bounds a tenth of what they
// are, which is what "the bound is set too low" looks like.
static BuildLockStallAction DecideLoosened(const BuildLockStallInputs& in)
{
	if (in.granted)
		return BL_STALL_RESET;
	if (in.runSeconds < BUILD_LOCK_STALL_SECONDS / 10.0
	    || in.runLength < BUILD_LOCK_STALL_RUN / 10)
		return BL_STALL_PASS;
	return in.latched ? BL_STALL_THROTTLE : BL_STALL_LATCH;
}

static void TestHealthyRunsNeverLatch()
{
	// The busiest healthy second, and the longest healthy run at that rate.
	Check(Replay(230.0, 1.0, BuildLockStallDecide).latches == 0,
	      "the busiest healthy second does not latch");
	Check(Replay(230.0, 2.0, BuildLockStallDecide).latches == 0,
	      "the longest healthy run does not latch");

	// Three times the healthy rate, still inside the healthy duration.
	Check(Replay(690.0, 2.0, BuildLockStallDecide).latches == 0,
	      "a run three times the healthy rate does not latch inside two seconds");

	// A trickle that outlives the duration bound but is no spin at all.
	Check(Replay(2.0, 30.0, BuildLockStallDecide).latches == 0,
	      "two refusals a second for half a minute does not latch");
}

static void TestTheMeasuredStormLatchesAndIsBounded()
{
	Run r = Replay(15700.0, 75.0, BuildLockStallDecide);
	Check(r.latches == 1, "the measured storm latches exactly once");
	Check(r.throttles > 0, "the measured storm is throttled");

	// The bound in the terms it was asked for: once latched, every further
	// refusal pays the sleep, so the rate is the sleep's reciprocal whatever
	// the lock answers.
	double bounded = 1000.0 / (double)BUILD_LOCK_STALL_SLEEP_MS;
	Check(bounded < 230.0,
	      "the throttled rate is below the busiest healthy second, let alone the storm");

	// What it cost before the bound engaged: the attempts the decision let
	// through unthrottled.
	long unthrottled = (long)(15700.0 * 75.0) - r.throttles;
	Check(unthrottled < 100000,
	      "the bound engages inside the first seconds, not after the storm");
}

static void TestGrantEndsTheRun()
{
	Run r;
	for (int i = 0; i < 120000; ++i)
		Step(r, false, i * (1.0 / 15700.0), BuildLockStallDecide);
	Check(r.latched, "a storm latches");
	Step(r, true, 20.0, BuildLockStallDecide);
	Check(!r.latched, "a grant clears the latch");
	Check(r.length == 0, "a grant clears the run");

	long latchesBefore = r.latches;
	for (int i = 0; i < 100; ++i)
		Step(r, false, 20.0 + i * 0.001, BuildLockStallDecide);
	Check(r.latches == latchesBefore, "a short run after a grant does not latch again");
}

static void TestTheReplaysCanSeeALatch()
{
	// Without this the healthy checks above would pass for a rule that never
	// latches at all. Drop both bounds by a factor of ten and the busiest
	// healthy second latches, so the replays do carry the signal the shipped
	// rule is being asked not to raise.
	Check(Replay(230.0, 1.0, DecideLoosened).latches == 1,
	      "the healthy replay latches under bounds a tenth as wide");
	Check(Replay(230.0, 2.0, DecideLoosened).latches == 1,
	      "the longest healthy run latches under bounds a tenth as wide");
}

static void TestBoundsAreLoadBearing()
{
	// Each bound excludes a case the other admits, so neither can be dropped
	// at no cost.
	BuildLockStallInputs in;
	in.granted = false;
	in.latched = false;

	in.runLength  = BUILD_LOCK_STALL_RUN;
	in.runSeconds = BUILD_LOCK_STALL_SECONDS - 0.001;
	Check(BuildLockStallDecide(in) == BL_STALL_PASS, "the count alone does not latch");

	in.runLength  = BUILD_LOCK_STALL_RUN - 1;
	in.runSeconds = BUILD_LOCK_STALL_SECONDS * 10.0;
	Check(BuildLockStallDecide(in) == BL_STALL_PASS, "the duration alone does not latch");

	in.runLength  = BUILD_LOCK_STALL_RUN;
	in.runSeconds = BUILD_LOCK_STALL_SECONDS;
	Check(BuildLockStallDecide(in) == BL_STALL_LATCH, "both bounds together latch");
	in.latched = true;
	Check(BuildLockStallDecide(in) == BL_STALL_THROTTLE, "a latched run throttles");
}


// A site's whole run, driven the way the hook drives it, with the sleeps the
// site would actually take counted.
struct SiteRun
{
	Run  run;
	long sleeps;
	long latches;
	SiteRun() : sleeps(0), latches(0) {}
};

static void StepSite(SiteRun& s, BuildLockStallSite site, bool granted, double now)
{
	BuildLockStallInputs in;
	in.granted    = granted;
	in.runLength  = 0;
	in.runSeconds = 0.0;
	in.latched    = s.run.latched;
	if (!granted)
	{
		if (s.run.length == 0)
			s.run.startedAt = now;
		s.run.length++;
		in.runLength  = s.run.length;
		in.runSeconds = now - s.run.startedAt;
	}

	BuildLockStallDecision d = BuildLockStallEvaluate(in, site);
	if (d.action == BL_STALL_RESET)
	{
		s.run.length = 0; s.run.startedAt = 0.0; s.run.latched = false;
	}
	else if (d.action == BL_STALL_LATCH)
	{
		s.run.latched = true;
		s.latches++;
	}
	if (d.sleep)
		s.sleeps++;
}

// The storm shape, one site at a time: 5,000 refusals a second for 20 s, which
// is the unload loop's share of the measured 15,000/s.
static SiteRun ReplaySite(BuildLockStallSite site)
{
	SiteRun s;
	const double rate = 5000.0;
	for (int i = 0; i < (int)(rate * 20.0); ++i)
		StepSite(s, site, false, i / rate);
	return s;
}

static void TestAHeldRegionNeverSleeps()
{
	// The add loop asks with the change mutex held exclusively and the
	// generator's save loop asks on the generation thread. Both must report
	// the jam and neither may wait inside it.
	SiteRun add = ReplaySite(BL_SITE_UPDATE_ADD);
	Check(add.latches == 1, "a jam in the add loop is reported once");
	Check(add.sleeps == 0, "the add loop never sleeps inside the change region");

	SiteRun save = ReplaySite(BL_SITE_GEN_SAVE);
	Check(save.latches == 1, "a jam in the generator's save loop is reported once");
	Check(save.sleeps == 0, "the generator's save loop never sleeps");

	SiteRun other = ReplaySite(BL_SITE_UNKNOWN);
	Check(other.sleeps == 0, "an unrecognised caller never sleeps");
}

static void TestTheFreeLoopsStillThrottle()
{
	// The message drain holds nothing, and these are the loops that re-queue
	// the refused item and ask again forever. The bound has to still bite.
	SiteRun unload = ReplaySite(BL_SITE_UNLOAD);
	Check(unload.latches == 1, "a jam in the unload loop is reported once");
	Check(unload.sleeps > 0, "the unload loop still throttles");

	SiteRun create = ReplaySite(BL_SITE_CREATE);
	Check(create.sleeps > 0, "the createZone loop still throttles");

	// Each site clears its own bound on its own: the unload loop alone runs
	// far past 1000 refusals inside 5 s in the measured storm.
	Check(unload.run.length > BUILD_LOCK_STALL_RUN,
	      "one site alone reaches the run bound in the storm shape");
}

static void TestSitesDoNotShareARun()
{
	// A grant on one loop says nothing about another, and two trickles must
	// not add up into a bound neither of them reached.
	SiteRun add, unload;
	for (int i = 0; i < 900; ++i)
	{
		StepSite(add, BL_SITE_UPDATE_ADD, false, i * 0.005);
		StepSite(unload, BL_SITE_UNLOAD, false, i * 0.005);
	}
	Check(add.latches == 0 && unload.latches == 0,
	      "two sites under the count bound do not latch by summing");

	for (int i = 0; i < 2000; ++i)
		StepSite(unload, BL_SITE_UNLOAD, false, 4.5 + i * 0.005);
	Check(unload.latches == 1, "the unload loop latches on its own run");
	StepSite(add, BL_SITE_UPDATE_ADD, true, 20.0);
	Check(unload.run.latched, "a grant on another site does not clear this one");
}

int main()
{
	TestHealthyRunsNeverLatch();
	TestTheMeasuredStormLatchesAndIsBounded();
	TestGrantEndsTheRun();
	TestTheReplaysCanSeeALatch();
	TestBoundsAreLoadBearing();
	TestAHeldRegionNeverSleeps();
	TestTheFreeLoopsStillThrottle();
	TestSitesDoNotShareARun();

	return CheckExit("buildlock_stall_units");
}
