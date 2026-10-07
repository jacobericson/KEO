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
// the body's state; WOKE when conscious before it; NONE for an empty entry (expiry <= 0); STALE
// when the expiry lies further ahead than the cap (capHours, plus a minute) allows, as after a
// load into an earlier game time.
enum ThrowoutHold { TH_NONE = 0, TH_HELD, TH_WOKE, TH_CAP, TH_STALE };
ThrowoutHold ThrowoutHoldDecide(double nowHours, double expiryHours, bool unconscious, double capHours);
double ThrowoutHoldExpiry(double nowHours, int holdMinutes);   // now + minutes / 60

// The finder's mode, decided once per process: with another plugin's finder detour possibly in
// the chain, call the original and filter a held result; otherwise replace the loop.
enum ThrowoutFinderMode { TFM_REPLACE = 0, TFM_CHAIN };
ThrowoutFinderMode ThrowoutFinderModeFor(bool otherFinderLoaded);
// The mode from the install until the tick's first call decides it: the original chained.
ThrowoutFinderMode ThrowoutFinderModeBeforeTick();

// In chain mode, a held result is replaced by the replacement loop's answer only while no live
// foreign detour sits in the finder's chain; otherwise it is answered as no candidate.
bool ThrowoutChainFallback(bool heldResult, bool foreignHopLive);

// One hop of the walk down the finder's entry chain. OURS: our own detour, the walk continues
// from our trampoline. FOLLOW: a jump, followed. END: vanilla code (the exe, or memory no module
// owns, which holds a trampoline's copy of vanilla's prologue). LIVE: another module's code, or
// bytes that cannot be read.
enum ThrowoutHopKind { THK_FOLLOW = 0, THK_OURS, THK_END, THK_LIVE };
ThrowoutHopKind ThrowoutClassifyHop(bool readable, bool isJump, bool inExe, bool inOurs, bool inOtherModule);
const int kThrowoutMaxHops = 12;  // a longer walk counts as live
// The target of the jump at `at` (E9 rel32, or FF 25 disp32 through the pointer `slot` read
// there), or 0 when `bytes` is not one. `slot` is the pointer an FF 25 reads, ignored for E9.
unsigned __int64 ThrowoutJumpTarget(const unsigned char* bytes, unsigned __int64 at, unsigned __int64 slot);
// The address an FF 25 at `at` reads its pointer from, or 0 when `bytes` is not FF 25.
unsigned __int64 ThrowoutJumpSlot(const unsigned char* bytes, unsigned __int64 at);

// A task vftable entry reaches `expect`: it is `expect`, or an E9 rel32 thunk to it.
bool ThrowoutSlotResolves(const unsigned char* entryBytes, unsigned __int64 entry, unsigned __int64 expect);

const int kThrowoutInPrison = 2;   // UseStuffState IN_PRISON
// Vanilla's five conditions in its order, then the hold: a seen, uncarried, unconscious,
// non-prisoner character in this town whose walls flag says inside, and not held.
bool ThrowoutCandidate(bool haveChar, bool beingCarried, bool sameTown, bool unconscious,
                       int inSomething, int insideWalls, bool held);

#endif // KEO_FIXES_THROWOUT_POLICY_H
