#include <cstdio>
#include "movement/k7_arrival_policy.h"

#include "check.h"

int main()
{
	const int READY    = K7_ARRIVAL_ZR_BUILDINGS_PENDING;
	const int NOT_IN   = K7_ARRIVAL_ZR_NOT_IN_WORLD;
	const int UNKNOWN  = 4;   // ZR_UNKNOWN
	const float FAR    = 1200.0f * 1200.0f;
	const float NEAR   = 500.0f * 500.0f;

	// -------------------------------------------------------------------
	// K7ArrivalShouldArm
	// -------------------------------------------------------------------
	Check(K7ArrivalShouldArm(FAR, NOT_IN, -1.0),
	      "arm: far + not-in-world now -> arm");
	Check(!K7ArrivalShouldArm(FAR, READY, -1.0),
	      "arm: cell in the world, never seen not-in -> do not arm");
	Check(!K7ArrivalShouldArm(NEAR, NOT_IN, -1.0),
	      "arm: dDest <= 1000 -> do not arm");
	Check(!K7ArrivalShouldArm(FAR, UNKNOWN, -1.0),
	      "arm: unknown class, never seen not-in -> do not arm");
	Check(!K7ArrivalShouldArm(1000.0f * 1000.0f, NOT_IN, -1.0),
	      "arm: exactly 1000 units -> do not arm (strict >)");
	Check(K7ArrivalShouldArm(1000.1f * 1000.1f, NOT_IN, -1.0),
	      "arm: just over 1000 units -> arm");
	// A recent not-in -> in transition still arms. The window
	// (K7_ARRIVAL_RECENT_TRANSITION, 4.0 s, inclusive) leaves margin over a
	// +/-1 s clock uncertainty in the recorded stops and k7DestLastNotIn's
	// own up-to-0.25s poll lag.
	Check(K7ArrivalShouldArm(FAR, READY, 0.6),
	      "arm: cell in the world, transitioned 0.6s ago -> arm (recent)");
	Check(K7ArrivalShouldArm(FAR, READY, 4.0),
	      "arm: transitioned exactly 4.0s ago -> arm (inclusive)");
	Check(!K7ArrivalShouldArm(FAR, READY, 4.1),
	      "arm: transitioned 4.1s ago -> too old, do not arm");
	Check(!K7ArrivalShouldArm(FAR, READY, 6.0),
	      "arm: transitioned 6.0s ago -> too old, do not arm");

	// -------------------------------------------------------------------
	// K7ArrivalArmEdge
	// -------------------------------------------------------------------
	Check(K7ArrivalArmEdge(false, true, false, false, FAR, NOT_IN, -1.0),
	      "edge: rising signal, far, not in -> arm");
	Check(!K7ArrivalArmEdge(true, true, false, false, FAR, NOT_IN, -1.0),
	      "edge: steady signal (no rising edge) -> no re-arm");
	Check(!K7ArrivalArmEdge(false, true, true, false, FAR, NOT_IN, -1.0),
	      "edge: formation still gathering -> no arm (B1)");
	// The E1/E3/E4 shape: the cell is already in at the stop, but recently.
	Check(K7ArrivalArmEdge(false, true, false, false, FAR, READY, 2.0),
	      "edge: cell already in, recent transition (2s) -> armed (B2 option a)");
	Check(!K7ArrivalArmEdge(false, true, false, false, FAR, READY, 6.0),
	      "edge: cell already in, transition too old (6s) -> not armed");
	Check(!K7ArrivalArmEdge(false, false, false, false, FAR, NOT_IN, -1.0),
	      "edge: no signal at all -> no arm");
	// Defense-in-depth only: k7_observe.cpp never calls this with
	// paused=true (IslandReissuePollTick returns before K7SampleSignatures
	// runs at all while paused), so this does not by itself prove "no arm
	// while paused" -- that property rests entirely on the early return,
	// which is not host-testable (it lives in a game file). This just checks
	// the guard does what it says if it is ever reached.
	Check(!K7ArrivalArmEdge(false, true, false, true, FAR, NOT_IN, -1.0),
	      "edge: paused -> refuses to arm if called (not the real guard; see comment)");

	// -------------------------------------------------------------------
	// K7ArrivalPoll: fire on the transition
	// -------------------------------------------------------------------
	Check(K7ArrivalPoll(READY, 0.4, 15.0) == K7_ARRIVAL_FIRE,
	      "poll: in-world on the very first poll -> fire");
	Check(K7ArrivalPoll(NOT_IN, 0.4, 15.0) == K7_ARRIVAL_WAIT,
	      "poll: still not-in-world, well under the cap -> wait");
	Check(K7ArrivalPoll(UNKNOWN, 0.4, 15.0) == K7_ARRIVAL_WAIT,
	      "poll: unknown class, under the cap -> wait");

	// -------------------------------------------------------------------
	// K7ArrivalPoll: expire at 15 s
	// -------------------------------------------------------------------
	Check(K7ArrivalPoll(NOT_IN, 15.0, 15.0) == K7_ARRIVAL_EXPIRE,
	      "poll: exactly at the cap, still not-in-world -> expire");
	Check(K7ArrivalPoll(NOT_IN, 14.9, 15.0) == K7_ARRIVAL_WAIT,
	      "poll: just under the cap -> still waiting");
	Check(K7ArrivalPoll(UNKNOWN, 20.0, 15.0) == K7_ARRIVAL_EXPIRE,
	      "poll: past the cap while unknown -> expire");
	Check(K7ArrivalPoll(READY, 30.0, 15.0) == K7_ARRIVAL_FIRE,
	      "poll: in-world even past the cap -> fire, not expire (K7TryArrivalReissue's own N3 gate handles a refused fire)");

	// -------------------------------------------------------------------
	// K7ArrivalFireGate (8 cases, extended with destMatch/zonesOk)
	// -------------------------------------------------------------------
	Check(K7ArrivalFireGate(true,  false, true,  true, true, false, false, 0, 8) == K7_ARR_SEND,
	      "gate: enabled, all clear -> send");
	Check(K7ArrivalFireGate(true,  false, true,  true, true, false, true,  0, 8) == K7_ARR_REFUSE,
	      "gate: cooldown -> refuse");
	Check(K7ArrivalFireGate(true,  false, true,  true, true, false, false, 8, 8) == K7_ARR_REFUSE,
	      "gate: budget spent -> refuse");
	Check(K7ArrivalFireGate(true,  false, true,  true, true, true,  false, 0, 8) == K7_ARR_REFUSE,
	      "gate: character state blocks -> refuse");
	Check(K7ArrivalFireGate(true,  false, false, true, true, false, false, 0, 8) == K7_ARR_REFUSE,
	      "gate: walking again (not still stopped) -> refuse");
	Check(K7ArrivalFireGate(true,  true,  true,  true, true, false, false, 0, 8) == K7_ARR_NONE,
	      "gate: held -> nothing, even though enabled");
	Check(K7ArrivalFireGate(false, false, true,  true, true, false, false, 0, 8) == K7_ARR_LATCH_OBSERVE,
	      "gate: key false -> latch, never send");
	Check(K7ArrivalFireGate(false, true,  true,  true, true, false, false, 0, 8) == K7_ARR_NONE,
	      "gate: held wins even when the key is false");
	// N2b: the destination-match and zone-accessibility gates.
	Check(K7ArrivalFireGate(true,  false, true,  false, true, false, false, 0, 8) == K7_ARR_REFUSE,
	      "gate: destination no longer matches (+0xDC) -> refuse");
	Check(K7ArrivalFireGate(true,  false, true,  true, false, false, false, 0, 8) == K7_ARR_REFUSE,
	      "gate: zones not accessible -> refuse");
	// R4: refusal gates are evaluated regardless of `enabled`, so a
	// k7ArrivalTrigger=false poll that a refusal gate would also have
	// refused never latches a would-fire time.
	Check(K7ArrivalFireGate(false, false, false, true, true, false, false, 0, 8) == K7_ARR_REFUSE,
	      "gate: key false + not still stopped -> refuse, not latch (savedMs must not overstate)");

	return CheckExit("k7_arrival_units");
}
