#ifndef KEO_FIXES_THROWOUT_POLICY_H
#define KEO_FIXES_THROWOUT_POLICY_H

// Pure decisions for the throw-out fix: the walls flag at a drop, which drops are throw-outs and
// why, the hold's expiry, and the finder's candidate test. Host-tested; any thread.

// Whether the drop may ask the game for a gate code at all: not indoors, the gates singleton
// exists, and the navmesh exists and has not been stopped (the code lookup dereferences it).
bool ThrowoutMayAskGateCode(bool indoors, bool gatesPresent, bool navmeshPresent, bool navmeshStopped);

// The flag write at a drop. An indoors body keeps what it has; an unresolved code (-1: not asked,
// a gate pass running, a contended or missing face) writes nothing; any other code is written.
enum ThrowoutFlag { TF_INDOORS = 0, TF_UNRESOLVED, TF_WRITE };
ThrowoutFlag ThrowoutFlagDecide(bool indoors, int gateCode);

// The thread-local reason a throw-out task slot sets around its original.
enum ThrowoutReason { TR_NONE = 0, TR_TICK, TR_PATH_IMPOSSIBLE };
// The two slots are shared with the building throw-out task (Task_TakeIntruderOutside), whose
// drops leave no loop for the town's finder to break: a slot's drop is a town throw-out, and
// takes a hold, only when the task is the town one.
bool ThrowoutTownDrop(int reason, bool townTask);
// The tick slot's own 5 in-game-minute test: a stamp is set and five minutes have passed.
bool ThrowoutTimedOut(double stampHours, double nowHours);

enum ThrowoutDrop { TD_NONE = 0, TD_REACHED_OUTSIDE, TD_REACHED_INSIDE, TD_TIMEOUT, TD_PATH_IMPOSSIBLE };
// A drop that is not proven outside (written code 0) counts as inside.
ThrowoutDrop ThrowoutClassifyDrop(int reason, bool timedOut, ThrowoutFlag flag, int gateCode);

// The hold. HELD while unconscious and before the expiry; CAP once the expiry is reached, whatever
// the body's state; WOKE when conscious before it; NONE for an empty entry (expiry <= 0).
enum ThrowoutHold { TH_NONE = 0, TH_HELD, TH_WOKE, TH_CAP };
ThrowoutHold ThrowoutHoldDecide(double nowHours, double expiryHours, bool unconscious);
double ThrowoutHoldExpiry(double nowHours, int holdMinutes);   // now + minutes / 60

// The finder's mode, decided once per process: with another plugin's finder detour possibly in
// the chain, call the original and filter a held result; otherwise replace the loop.
enum ThrowoutFinderMode { TFM_REPLACE = 0, TFM_CHAIN };
ThrowoutFinderMode ThrowoutFinderModeFor(bool otherFinderLoaded);

const int kThrowoutInPrison = 2;   // UseStuffState IN_PRISON
// Vanilla's five conditions in its order, then the hold: a seen, uncarried, unconscious,
// non-prisoner character in this town whose walls flag says inside, and not held.
bool ThrowoutCandidate(bool haveChar, bool beingCarried, bool sameTown, bool unconscious,
                       int inSomething, int insideWalls, bool held);

#endif // KEO_FIXES_THROWOUT_POLICY_H
