#pragma once

// Decision logic behind the Ogre resource-purge skip (ogre_purge_skip.cpp).
// Pure arithmetic, host-testable and independent of Windows, Ogre or game
// headers: the caller decides "eligible" from the zone-handoff ledger and
// isTransitionActive, and keeps the clock in whatever unit it likes (seconds
// here) and the last-run timestamp across calls.

enum OgrePurgeAction
{
	OGREPURGE_RUN,   // call the original purge as normal
	OGREPURGE_SKIP   // leave this call out
};

struct OgrePurgeDecision
{
	OgrePurgeAction action;
	bool            countSkip;      // this call is a skip
	bool            countFallback;  // this call is a run forced by the time bound
};

// eligible: false for a real transition's purge, which always runs (a real
// transition is never something this skip touches). true for a purge call
// inside a loading cycle the handoff mechanism raised with no camera
// transition joined -- the caller's own definition, not decided here.
//
// While eligible, the call is skipped unless more than maxSkipSeconds have
// passed since the last run (RUN or fallback alike): elapsed == maxSkipSeconds
// still skips, since the bound is stated as "more than". maxSkipSeconds <= 0
// disables the fallback: an eligible call is then always skipped, so a
// caller wiring up the INI's default must pass a positive value to get the
// bound described by adoptOgrePurgeMaxSkipSeconds.
OgrePurgeDecision OgrePurgeSkipDecide(bool eligible, double now, double lastRunAt, double maxSkipSeconds);
