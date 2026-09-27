#ifndef KENSHI_ZONE_OPT_ORDER_OUTCOME_H
#define KENSHI_ZONE_OPT_ORDER_OUTCOME_H

#include "base/config.h"
#include <sstream>

// Player long-order outcome tracking: one record per player move order,
// closed and printed as an "OrderOutcome:" line when every member has
// arrived, gone unconscious, left the squad, been cancelled, or the record's
// safety timeout passes. Main thread only, driven from the same per-frame
// poll that already owns PLAYER STUCK and IslandTick (camera_zone_hook.cpp's
// updateCameraZone handler) -- never from a hook detour. Printed in every
// build variant and every INI: it is an instrument, not a lever.
//
// This file is the game-facing glue: it translates Character* and the
// game's own signals into calls on order_outcome_table.h's pure record
// table, which does the actual bookkeeping and classification
// (order_outcome_policy.h). Every function below that measures elapsed time
// reads zone_pause.h's ZonePauseIsPaused() itself and forwards it, so a
// paused game never advances a stall clock, the 2s floor or the safety
// timeout -- callers need not know or care about pause state.

void OrderOutcomeReset();

// A new player move order for chars[0..count) (order_hook.cpp, the same
// task==29 site as IslandNoteOrder). Any of these characters' previous
// records close first: an open stall there resolves as a user re-order.
void OrderOutcomeBegin(const uintptr_t* chars, int count, float destX, float destZ, double now);

// PLAYER STUCK's per-poll motion sample (player_task_snap.cpp), taken every
// poll -- moving or not -- ahead of its own zero-velocity bookkeeping.
// `post` is a pure position test against the order's own destination,
// terminal for this member the first time it reads true, whatever `moving`
// says.
void OrderOutcomeNoteMotion(uintptr_t character, bool moving, bool post, double now);

// `character` is unconscious or dead right now: terminal, like arrival --
// its part of the order is over, and it will not re-open a stall of its own
// on waking up with no order.
void OrderOutcomeNoteKo(uintptr_t character, double now);

// A K7-family re-issue was actually sent for `character`
// (island_reissue.cpp ReissueOrder's one success point, every reason),
// tagged with its form ("arr"/"del"/"park"/"growth"/"retry").
void OrderOutcomeNoteReissueSent(uintptr_t character, const char* form, double now);

// A stop-key, job order or other cancel ended `character`'s participation in
// its current move order (order_hook.cpp's non-move branch, the K7 cancel
// detours): a stall already open resolves as a user re-order, not a failure.
void OrderOutcomeCancel(uintptr_t character, double now);

// A formation group completed (formation.cpp): close and print any order
// record shared by these characters that member-level retirement (arrival,
// knockout, cancel, squad departure) has not already closed.
void OrderOutcomeOnGroupComplete(const uintptr_t* chars, int count, double now);

// Per-frame housekeeping (IslandTick): drops a member no longer in the
// player squad, and closes a record whose members are all gone or whose age
// passes the safety timeout.
void OrderOutcomePoll(double now);

// stops=' class guess for `character`'s current stall (PLAYER STUCK's own
// k7=/span@arr= fields): latched once per stop, and only when the caller has
// already confirmed the stall is not excluded (ko/post).
void OrderOutcomeNoteStopGuess(uintptr_t character, const char* guess, double now);

// The PLAYER STUCK suffix itself is a pure formatter
// (order_outcome_policy.h's OrderOutcomeStuckSuffix) -- player_task_snap.cpp
// calls it directly and passes the same ko/post it hands NoteMotion/NoteKo.

// Appends " orders=<n> longOrders=<n> longStop=<n> longFail=<n> userRec=<n>
// unrec=<n>" to the PROD-visible IslandSpan: line and reports whether any of
// the totals moved since the last call, so the line's own change gate can
// include it.
bool OrderOutcomeAppendSpanTotals(std::ostringstream& ss);

#endif // KENSHI_ZONE_OPT_ORDER_OUTCOME_H
