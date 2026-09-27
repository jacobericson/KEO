#include <cstdio>
#include "zone/reset/zone_reset_gate.h"

#include "check.h"

// Single-threaded: the waits that must end are ended by their own stop
// callback, which on its second call either reports the stop or lowers the
// gate. The first call is the wait's decision before it counts.

static ZoneResetGate s_gate;
static HANDLE        s_parked;

static int  s_stopCalls;
static bool NeverStopped() { ++s_stopCalls; return false; }
static bool AlwaysStopped() { ++s_stopCalls; return true; }
static bool StoppedOnSecondCall() { return ++s_stopCalls >= 2; }
static bool LowersOnSecondCall()
{
	if (++s_stopCalls == 2)
		ZoneResetGateLower(&s_gate);
	return false;
}

static bool Signalled(HANDLE h)
{
	return h != NULL && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
}

static bool NothingCounted(const ZoneResetGate* g)
{
	return ZoneResetGateDeferredTotal(g) == 0
	    && ZoneResetGateDeferredAt(g, ZONE_RESET_SITE_CLAIM) == 0
	    && ZoneResetGateDeferredAt(g, ZONE_RESET_SITE_HIT) == 0
	    && ZoneResetGateDeferredAt(g, ZONE_RESET_SITE_BUILD) == 0;
}

static int s_raiseUnderCalls;
static void RaiseUnder(ZoneResetGate* g)
{
	++s_raiseUnderCalls;
	ZoneResetGateRaise(g);
}

static int  s_afterLowerCalls;
static bool s_upAtAfterLower;
static bool s_overSetAtAfterLower;
static void AfterLower()
{
	++s_afterLowerCalls;
	s_upAtAfterLower      = ZoneResetGateUp(&s_gate);
	s_overSetAtAfterLower = Signalled(s_gate.over);
}

enum Script
{
	SCRIPT_WHOLE_AFTER_WAIT,
	SCRIPT_LOWER_AT_ENTRY,
	SCRIPT_LOWER_IN_WAIT,
	SCRIPT_PARTIAL_RAISE,
	SCRIPT_STOP_AFTER_WAIT,
	SCRIPT_STOP_AT_FINAL
};

static Script s_script;
static int s_scriptCalls;
static int s_scriptPhase;
static bool ScriptStop()
{
	int call = ++s_scriptCalls;
	if (s_script == SCRIPT_WHOLE_AFTER_WAIT && call == 2)
	{
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		s_scriptPhase = 1;
	}
	else if (s_script == SCRIPT_LOWER_AT_ENTRY && call == 1)
	{
		ZoneResetGateLower(&s_gate);
		s_scriptPhase = 1;
	}
	else if (s_script == SCRIPT_LOWER_IN_WAIT && call == 3)
	{
		ZoneResetGateLower(&s_gate);
		s_scriptPhase = 1;
	}
	else if (s_script == SCRIPT_PARTIAL_RAISE && call == 2)
	{
		InterlockedExchange(&s_gate.deferredTotal, 0);
		for (int i = 0; i < ZONE_RESET_SITE_COUNT; ++i)
			InterlockedExchange(&s_gate.deferred[i], 0);
		ResetEvent(s_gate.over);
		InterlockedExchange(&s_gate.inProgress, 1);
		s_scriptPhase = 1;
	}
	else if (s_script == SCRIPT_PARTIAL_RAISE && call == 3)
	{
		InterlockedIncrement(&s_gate.raises);
		ZoneResetGateLower(&s_gate);
		s_scriptPhase = 2;
	}
	else if (s_script == SCRIPT_STOP_AFTER_WAIT && call == 3)
	{
		ZoneResetGateLower(&s_gate);
		s_scriptPhase = 1;
	}
	if (s_script == SCRIPT_STOP_AFTER_WAIT && call >= 4)
	{
		s_scriptPhase = 2;
		return true;
	}
	if (s_script == SCRIPT_STOP_AT_FINAL && call == 3)
	{
		s_scriptPhase = 1;
		return true;
	}
	return false;
}

static void StartScript(Script script)
{
	s_script = script;
	s_scriptCalls = 0;
	s_scriptPhase = 0;
}

static int s_lowerUnderCalls;
static bool s_lowerSawUp;
static void LowerUnder(ZoneResetGate* g)
{
	++s_lowerUnderCalls;
	s_lowerSawUp = ZoneResetGateUp(g);
	ZoneResetGateLower(g);
}

int main()
{
	s_parked = CreateEvent(NULL, TRUE, FALSE, NULL);
	s_gate.parked = s_parked;

	{
		bool made = ZoneResetGateInit(&s_gate);
		CHECK(made && !ZoneResetGateUp(&s_gate) && Signalled(s_gate.over) && NothingCounted(&s_gate)
			&& ZoneResetGateRaises(&s_gate) == 0,
			"gate: down after init, with its event set");
	}

	{
		// One refusal counted while down, so "counts nothing" is measured
		// against a non-zero total.
		ZoneResetGateNoteDeferred(&s_gate, ZONE_RESET_SITE_CLAIM);
		s_stopCalls = 0;
		ZoneResetWait w = ZoneResetGateWait(&s_gate, ZONE_RESET_SITE_HIT, &NeverStopped);
		CHECK(w == ZONE_RESET_WAIT_NONE && s_stopCalls == 0
			&& ZoneResetGateDeferredTotal(&s_gate) == 1
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_CLAIM) == 1
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_HIT) == 0
			&& !Signalled(s_parked) && Signalled(s_gate.over),
			"gate: a wait while down admits at once and counts nothing");
	}

	{
		ZoneResetGateNoteDeferred(&s_gate, ZONE_RESET_SITE_BUILD);
		bool countedBefore = ZoneResetGateDeferredTotal(&s_gate) == 2;
		ZoneResetGateRaise(&s_gate);
		CHECK(countedBefore && ZoneResetGateUp(&s_gate) && NothingCounted(&s_gate)
			&& s_gate.over != NULL && !Signalled(s_gate.over),
			"gate: raise zeroes the counts and resets the event before the gate reads up");
	}

	{
		s_stopCalls = 0;
		ZoneResetWait w = ZoneResetGateWait(&s_gate, ZONE_RESET_SITE_BUILD, &AlwaysStopped);
		CHECK(w == ZONE_RESET_WAIT_STOPPED && s_stopCalls == 1 && ZoneResetGateUp(&s_gate)
			&& NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate: a wait while up with the stop seen returns stopped at once and counts nothing");
	}

	{
		// Counted, parked, one slice on the unsignalled event, then the stop.
		s_stopCalls = 0;
		ZoneResetWait w = ZoneResetGateWait(&s_gate, ZONE_RESET_SITE_HIT, &StoppedOnSecondCall);
		bool waitCounted = w == ZONE_RESET_WAIT_STOPPED && s_stopCalls == 2
			&& Signalled(s_parked)
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_HIT) == 1
			&& ZoneResetGateDeferredTotal(&s_gate) == 1;
		ZoneResetGateNoteDeferred(&s_gate, ZONE_RESET_SITE_CLAIM);
		CHECK(waitCounted
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_CLAIM) == 1
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_HIT) == 1
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_BUILD) == 0
			&& ZoneResetGateDeferredTotal(&s_gate) == 2,
			"gate: a refusal counts at its site and in the total");
		ResetEvent(s_parked);
	}

	{
		const ZoneResetSite outside = (ZoneResetSite)ZONE_RESET_SITE_COUNT;
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateNoteDeferred(&s_gate, outside);
		s_stopCalls = 0;
		ZoneResetWait w = ZoneResetGateWait(&s_gate, outside, &StoppedOnSecondCall);
		CHECK(w == ZONE_RESET_WAIT_STOPPED && ZoneResetGateDeferredTotal(&s_gate) == 2
			&& ZoneResetGateDeferredAt(&s_gate, outside) == 0
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_CLAIM) == 0
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_HIT) == 0
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_BUILD) == 0,
			"gate: a site outside the table counts in the total only");
		ResetEvent(s_parked);
	}

	{
		// A lower during a committed wait: the next slice reads the gate down
		// and the waiter is admitted as having waited.
		ZoneResetGateRaise(&s_gate);
		s_stopCalls = 0;
		ZoneResetWait w = ZoneResetGateWait(&s_gate, ZONE_RESET_SITE_BUILD, &LowersOnSecondCall);
		bool woke = w == ZONE_RESET_WAIT_WAITED && s_stopCalls == 2
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_BUILD) == 1;
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		CHECK(woke && !ZoneResetGateUp(&s_gate) && Signalled(s_gate.over),
			"gate: lower sets the event after the gate reads down");
		ResetEvent(s_parked);
	}

	{
		s_raiseUnderCalls = 0;
		bool upThrough = false, upDirect = false;
		{
			ZoneResetGateScope scope(&s_gate, &RaiseUnder, NULL);
			upThrough = ZoneResetGateUp(&s_gate) && s_raiseUnderCalls == 1;
		}
		bool downBetween = !ZoneResetGateUp(&s_gate);
		{
			ZoneResetGateScope scope(&s_gate, NULL, NULL);
			upDirect = ZoneResetGateUp(&s_gate) && !Signalled(s_gate.over);
		}
		CHECK(upThrough && downBetween && upDirect && s_raiseUnderCalls == 1,
			"gate: the scope raises through the function it is given, and directly without one");
	}

	{
		s_afterLowerCalls = 0;
		s_upAtAfterLower = true;
		s_overSetAtAfterLower = false;
		bool upInside = false;
		{
			ZoneResetGateScope scope(&s_gate, NULL, &AfterLower);
			upInside = ZoneResetGateUp(&s_gate);
		}
		CHECK(upInside && s_afterLowerCalls == 1 && !s_upAtAfterLower && s_overSetAtAfterLower
			&& !ZoneResetGateUp(&s_gate),
			"gate: the scope lowers on exit, then calls its after-lower function");
	}

	{
		s_afterLowerCalls = 0;
		s_upAtAfterLower = true;
		int caught = 0;
		bool upInside = false;
		try
		{
			ZoneResetGateScope scope(&s_gate, &RaiseUnder, &AfterLower);
			upInside = ZoneResetGateUp(&s_gate);
			if (upInside)
				throw 7;
		}
		catch (int e)
		{
			caught = e;
		}
		CHECK(upInside && caught == 7 && !ZoneResetGateUp(&s_gate) && Signalled(s_gate.over)
			&& s_afterLowerCalls == 1 && !s_upAtAfterLower,
			"gate: the scope lowers on an unwind");
	}

	{
		LONG before = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		CHECK(ZoneResetGateRaises(&s_gate) == before + 1 && !ZoneResetGateUp(&s_gate),
			"gate since: a complete raise advances the count once");
	}

	{
		ResetEvent(s_parked);
		s_stopCalls = 0;
		long before = ZoneResetGateDeferredTotal(&s_gate);
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_HIT, &NeverStopped, claim);
		CHECK(w == ZONE_RESET_WAIT_NONE && s_stopCalls == 3
			&& ZoneResetGateDeferredTotal(&s_gate) == before && !Signalled(s_parked)
			&& !ZoneResetGateUp(&s_gate),
			"gate since: down with unchanged count returns none without refusal or parking");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, NULL, claim);
		CHECK(w == ZONE_RESET_WAIT_WAITED && NothingCounted(&s_gate)
			&& !Signalled(s_parked) && !ZoneResetGateUp(&s_gate),
			"gate since: a whole reset completed between claim and wait requires revalidation");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		StartScript(SCRIPT_WHOLE_AFTER_WAIT);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, &ScriptStop, claim);
		CHECK(w == ZONE_RESET_WAIT_WAITED && s_scriptPhase == 1 && s_scriptCalls >= 6
			&& NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate since: whole reset after the underlying down wait retries and revalidates");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		StartScript(SCRIPT_LOWER_AT_ENTRY);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, &ScriptStop, claim);
		ZoneResetGateLower(&s_gate);   // cleanup if the body ignored the script
		CHECK(w == ZONE_RESET_WAIT_WAITED && s_scriptPhase == 1
			&& NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate since: lower before the underlying wait still requires revalidation without refusal");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		StartScript(SCRIPT_LOWER_IN_WAIT);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, &ScriptStop, claim);
		CHECK(w == ZONE_RESET_WAIT_WAITED && s_scriptPhase == 1
			&& ZoneResetGateDeferredTotal(&s_gate) == 1
			&& ZoneResetGateDeferredAt(&s_gate, ZONE_RESET_SITE_BUILD) == 1
			&& Signalled(s_parked) && !ZoneResetGateUp(&s_gate),
			"gate since: lower inside a committed wait retains waited and one refusal");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		StartScript(SCRIPT_PARTIAL_RAISE);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, &ScriptStop, claim);
		ZoneResetGateLower(&s_gate);   // cleanup if the body returned early
		CHECK(w == ZONE_RESET_WAIT_WAITED && s_scriptPhase == 2 && s_scriptCalls >= 6
			&& NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate since: partial raise with old count sees up, retries, then revalidates");
	}

	{
		ResetEvent(s_parked);
		LONG claim = ZoneResetGateRaises(&s_gate);
		s_stopCalls = 0;
		ZoneResetWait equal = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_HIT, &AlwaysStopped, claim);
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		ZoneResetWait changed = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_HIT, &AlwaysStopped, claim);
		CHECK(equal == ZONE_RESET_WAIT_STOPPED && changed == ZONE_RESET_WAIT_STOPPED
			&& s_stopCalls == 2 && NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate since: stop at entry wins for equal and changed claim counts");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		StartScript(SCRIPT_STOP_AFTER_WAIT);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_BUILD, &ScriptStop, claim);
		CHECK(w == ZONE_RESET_WAIT_STOPPED && s_scriptPhase == 2
			&& ZoneResetGateDeferredTotal(&s_gate) == 1 && Signalled(s_parked),
			"gate since: stop after a committed wait wins without erasing its refusal");
	}

	{
		LONG claim = ZoneResetGateRaises(&s_gate);
		ZoneResetGateRaise(&s_gate);
		ZoneResetGateLower(&s_gate);
		StartScript(SCRIPT_STOP_AT_FINAL);
		ResetEvent(s_parked);
		ZoneResetWait w = ZoneResetGateWaitSince(&s_gate, ZONE_RESET_SITE_HIT, &ScriptStop, claim);
		CHECK(w == ZONE_RESET_WAIT_STOPPED && s_scriptPhase == 1 && s_scriptCalls == 3
			&& NothingCounted(&s_gate) && !Signalled(s_parked),
			"gate since: stop at the final down sample wins over changed count");
	}

	{
		s_lowerUnderCalls = s_afterLowerCalls = 0;
		s_lowerSawUp = false;
		{
			ZoneResetGateScope scope(&s_gate, NULL, &AfterLower, &LowerUnder);
		}
		CHECK(s_lowerUnderCalls == 1 && s_lowerSawUp && s_afterLowerCalls == 1
			&& !s_upAtAfterLower && s_overSetAtAfterLower && !ZoneResetGateUp(&s_gate),
			"gate since: scoped lower callback runs once before the after-lower wake");
		s_lowerUnderCalls = s_afterLowerCalls = 0;
		int caught = 0;
		try
		{
			ZoneResetGateScope scope(&s_gate, NULL, &AfterLower, &LowerUnder);
			throw 9;
		}
		catch (int e)
		{
			caught = e;
		}
		CHECK(caught == 9 && s_lowerUnderCalls == 1 && s_afterLowerCalls == 1
			&& !s_upAtAfterLower && s_overSetAtAfterLower && !ZoneResetGateUp(&s_gate),
			"gate since: scoped lower callback also runs once on unwind");
	}

	CloseHandle(s_parked);
	return CheckExit("zone_reset_gate_units");
}
