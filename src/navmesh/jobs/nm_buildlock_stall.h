#ifndef KENSHI_ZONE_OPT_NM_BUILDLOCK_STALL_H
#define KENSHI_ZONE_OPT_NM_BUILDLOCK_STALL_H

// When a non-blocking acquisition of the navmesh build lock is refused, the
// engine does not wait: it re-queues the work item and asks again on its next
// pass, for every item, forever. While the lock is held by work that will
// finish, the run of refusals ends on its own. While it is held by a thread
// that will never release it, the run has no end, and the asking costs a heap
// allocation and a log line each time.
//
// This decides when a run has stopped being a wait and started being a spin.
// No game state and no clock of its own: the caller counts the run and times
// it, this says what to do about it.

enum BuildLockStallAction
{
	BL_STALL_PASS     = 0,  // a refusal inside the ordinary wait
	BL_STALL_RESET    = 1,  // the lock was granted: the run is over
	BL_STALL_LATCH    = 2,  // the first refusal past the bound
	BL_STALL_THROTTLE = 3   // a later refusal past the bound
};

// Which loop asked. The answer decides whether waiting is allowed at all, so
// the sites are kept apart rather than merged into one spin.
//
// Two of the four ask with a lock or a pipeline behind them. The add loop asks
// with the section manager's change mutex held exclusively, and every
// navigability query try-locks that same mutex shared: sleeping there converts
// a spin into "no character can be ordered anywhere" for the length of the
// sleep. The generator's per-sector save loop asks on the generation thread,
// where a wait delays the meshes the spin is waiting for. Neither may sleep.
//
// The other two ask from the message drain, which holds nothing: the message
// queue's own mutex is released before the message is dispatched. Those are
// the loops that re-queue the refused item and ask again next pass forever,
// and they are where the bound belongs.
//
// An unrecognised site is another plugin's detour or a caller nobody has seen;
// it is counted and never slept on, because what it holds is unknown.
enum BuildLockStallSite
{
	BL_SITE_UNKNOWN    = 0,
	BL_SITE_UPDATE_ADD = 1,  // NavMesh::update's add loop, inside the change region
	BL_SITE_GEN_SAVE   = 2,  // NavMeshGenerator::update's per-sector save loop
	BL_SITE_UNLOAD     = 3,  // NavMesh::unloadZone, from the message drain
	BL_SITE_CREATE     = 4,  // NavMesh::createZone, from the message drain
	BL_SITE_COUNT      = 5
};

bool BuildLockStallSiteMaySleep(BuildLockStallSite site);

// The site's short name for the log line.
const char* BuildLockStallSiteName(BuildLockStallSite site);

struct BuildLockStallInputs
{
	bool   granted;     // what this acquisition answered
	long   runLength;   // refusals in the unbroken run, this one included
	double runSeconds;  // from the run's first refusal to this one
	bool   latched;     // the run has already been reported
};

// Both bounds are load-bearing and neither subsumes the other. The duration
// is what separates a jam from a wait: across fifty sessions of healthy play
// the longest unbroken run of refusals lasted two seconds, so five is beyond
// anything a live holder has produced. The count is what makes the bound a
// rate rather than a duration: without it a pair of refusals five seconds
// apart would latch, and slowing that down helps nothing.
extern const double BUILD_LOCK_STALL_SECONDS;
extern const long   BUILD_LOCK_STALL_RUN;

// Paid per refusal once latched, on the thread doing the asking. It caps the
// spin at a few dozen attempts a second whatever the lock answers.
extern const int    BUILD_LOCK_STALL_SLEEP_MS;

BuildLockStallAction BuildLockStallDecide(const BuildLockStallInputs& in);

// What the bound says, and whether this site is allowed to act on it. A site
// that may not sleep still latches and still reports: a jam there is a finding
// whether or not anything can be done about it from that thread.
struct BuildLockStallDecision
{
	BuildLockStallAction action;
	bool                 sleep;
};

BuildLockStallDecision BuildLockStallEvaluate(const BuildLockStallInputs& in, BuildLockStallSite site);

#endif // KENSHI_ZONE_OPT_NM_BUILDLOCK_STALL_H
