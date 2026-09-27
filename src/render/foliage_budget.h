#pragma once

// The per-frame admission rule of the foliage page-build budget. No Windows
// or game calls, so the host tests link it directly.
//
// Calls are numbered 0..N-1 each frame; they keep their numbers only while
// the caller's list (the game's active zone set, an unordered container)
// keeps the same members. A budgeted frame admits calls from `start` on
// while the time spent is under the budget; the first call it turns away
// becomes the next frame's start, and calls before `start` wait for the
// wrap. Call 0 and the call at `start` are always admitted: `start` keeps the
// rotation moving, and call 0 means a frame runs at least one call even when
// the list shrank below `start` since the last frame. On a stable list,
// with B calls admitted per frame besides call 0, every call runs within
// ceil(N / B) + 1 frames.
struct FoliageBudget
{
	int    start;       // first call index the next budgeted frame admits
	int    index;       // calls seen this frame
	int    resumeAt;    // first call turned away for budget this frame, -1 none
	bool   budgeted;    // some call this frame was under the budget rule
	double spentMs;     // admitted calls' time this frame
	double frameMaxMs;  // largest frame total since the last read
};

void FoliageBudgetReset(FoliageBudget* b);

// Called before each call. active: the budget applies to this call (game
// speed above the threshold, budget > 0). True when the call should run.
bool FoliageBudgetAdmit(FoliageBudget* b, bool active, double budgetMs);

// The time an admitted call took.
void FoliageBudgetSpend(FoliageBudget* b, double ms);

// Once per frame, after the frame's last call: picks the next start and
// clears the frame's counts.
void FoliageBudgetEndFrame(FoliageBudget* b);
