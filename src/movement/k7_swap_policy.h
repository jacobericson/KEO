#ifndef KEO_K7_SWAP_POLICY_H
#define KEO_K7_SWAP_POLICY_H

// Classifies a K7 "x" swap (the current task leaves ORDER_TYPE_MOVE with an
// empty order deque, after task 29 was seen) as a genuine engine drop or a
// swap that followed the tracked move's own end signature into combat. Pure
// arithmetic: no game headers, so it is host-tested directly
// (tools/tests/k7_swap_units.cpp).
//
// k7_observe.cpp and k7_reissue.cpp are the only callers. They own every game read (task
// type, the deque, +0xDC); this file only orders the timestamps it hands in.

// The end signature must fall within this long of the deletion (or of the
// hold's start) to still count as "the same episode". Shared with
// k7_reissue.cpp's own K7TryDeletedReissue sigTime test.
extern const double K7_SIG_WINDOW;

enum K7SwapVerdict
{
	K7_SWAP_DROP = 0,        // no end signature before the swap: today's behaviour
	K7_SWAP_HOLD,            // died first, a combat task, inside the hold window
	K7_SWAP_EXPIRED,         // died first, but the hold ran past holdMax
	K7_SWAP_DROP_NONCOMBAT   // died first, but the new task is not on the combat whitelist
};

// last29     = the last poll that saw task 29 (0 = never).
// sig        = k7SigOnset: the first frame of the end signature since that
//              poll (0 = none this episode). Never a "last frame the
//              signature held" value -- those refresh every frame a stopped
//              character keeps the signature, so they would always read
//              "just now" at the swap poll.
// deletedSince = the order's own deletion latch, if a hold already armed it
//              (0 = not armed yet).
// swapSeen   = the first poll that saw the new (non-29, non--1) task since
//              last29 (0 = not seen yet; callers only classify once this is set).
// now        = current poll time.
// curType    = the current task type.
// margin     = minimum gap (seconds) between the signature onset and the
//              swap being seen, so an AI-driven switch in the very same poll
//              the order ends does not count as "died first".
// holdMax    = maximum seconds a hold may run before it expires.
K7SwapVerdict K7ClassifySwap(double last29, double sig, double deletedSince,
                             double swapSeen, double now, int curType,
                             double margin, double holdMax);

// KenshiLib Enums.h TaskType whitelist: tasks the AI runs while fighting or
// recovering from one, worth holding a dead move order for.
bool K7IsCombatTask(int curType);

// The end-signature onset latch, one pure step per frame. Cleared at every
// task-29 poll (a live move resumes, so an old onset can never taint a later
// swap) and left alone otherwise -- in particular it does NOT clear when the
// signature stops holding this frame, so it survives to the poll that first
// sees the swap.
double K7SigOnsetStep(double onset, bool sigThisFrame, bool task29Poll, double now);

// Whether the deleted-order re-issue may send this poll given the
// destination cell's readiness class (a ZR_* value from
// zone_readiness_classify.h, passed as a plain int so this file needs no
// game header) and how long consecutive refusals have been waiting.
// cls == ZR_BUILDINGS_PENDING (2, "the outdoor instance is in the world")
// always allows; anything else allows only once waitedSec has reached
// maxWaitSec, so a cell that never gets an instance cannot strand the entry.
bool K7DestReadyAllows(int cls, double waitedSec, double maxWaitSec);

#endif // KEO_K7_SWAP_POLICY_H
