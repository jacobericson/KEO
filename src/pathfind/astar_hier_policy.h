// astar_hier_policy.h - The player hierarchical search's pure decisions: which search is whose,
// the call sequence of each mode, the output reset, and the counters' buckets.
// No game or Windows header; the game side binds the sequence through AstarHierOps.
#ifndef KEO_ASTAR_HIER_POLICY_H
#define KEO_ASTAR_HIER_POLICY_H

#include <stddef.h>

enum AstarHierMode { AHIER_OFF = 0, AHIER_OBSERVE = 1, AHIER_ON = 2 };
enum AstarHierOnCap { AHIER_CAP_RERUN = 0, AHIER_CAP_KEEP = 1 };
enum AstarHierSubject { AHIER_SUBJECT_OTHER = 0, AHIER_SUBJECT_PLAYER = 1, AHIER_SUBJECT_NPC = 2 };
enum AstarHierNext { AHIER_NEXT_KEEP = 0, AHIER_NEXT_RERUN = 1 };
enum AstarHierRefusal { AHIER_REFUSAL_NONE = 0, AHIER_REFUSAL_FALSE, AHIER_REFUSAL_TRUE, AHIER_REFUSAL_UNDECIDED };
enum AstarHierEvent { AHIER_EVENT_NONE = 0, AHIER_EVENT_RESCUE, AHIER_EVENT_FALSE_REFUSAL, AHIER_EVENT_NPC_FALSE, AHIER_EVENT_DIVERGE };

// FindPathInput and FindPathOutput fields the sequence reads or writes.
const size_t AHIER_IN_COST_MODIFIER = 0x70;   // non-NULL: a water cost modifier
const size_t AHIER_IN_HIER_FLAG     = 0x83;   // 1: the cluster-graph heuristic
const size_t AHIER_OUT_VISITED_SIZE = 0x18;
const size_t AHIER_OUT_PATH_SIZE    = 0x28;
const size_t AHIER_OUT_ITERATIONS   = 0x30;
const size_t AHIER_OUT_GOAL_INDEX   = 0x34;
const size_t AHIER_OUT_COST         = 0x38;
const size_t AHIER_OUT_STATUS       = 0x3C;   // u8, then the u8 termination cause
const unsigned AHIER_COST_UNSET     = 0x7F7FFFEEu;

struct AstarHierResult { int status; int cause; int iters; float cost; };

// What the sequence asks of the game. search makes one original call on the current input and
// output; resetOutput restores the scalars a second call needs; readOutput reads a result;
// now returns QPC ticks; npcSampleDue claims one NPC shadow and is called once per NPC
// instant refusal.
struct AstarHierOps
{
	void* ctx;
	void (*search)(void* ctx);
	void (*setHierByte)(void* ctx, unsigned char value);
	void (*resetOutput)(void* ctx);
	void (*readOutput)(void* ctx, AstarHierResult* out);
	long long (*now)(void* ctx);
	bool (*npcSampleDue)(void* ctx);
};

struct AstarHierRecord
{
	AstarHierSubject subject;
	int calls;                 // original calls made
	bool hierRan, vanRan;
	AstarHierResult hier;      // the first hierarchical result (a player's arm, or the NPC's own search)
	AstarHierResult van;       // the vanilla arm, when it ran
	AstarHierResult last;      // the result the caller receives
	long long hierTicks, vanTicks;
	bool losMasked;            // hier returned status 1, iterations 0, with a cost modifier
	bool rerun;                // on: the vanilla arm replaced the hierarchical result
	bool keptTerm;             // on: a hierarchical status 3 kept (playerHierOnCap=keep)
	bool npcShadow;            // observe: an NPC instant refusal was shadowed by vanilla
	AstarHierRefusal npcClass;
	bool ratioValid;
	float ratio;               // hier.cost / van.cost, both A* status 1
};

// Which counters one record moves. Every player search lands in exactly one of hierOk,
// losMasked, hierTerm, hierUnreach, hierInvalid and hierOther.
struct AstarHierBumps
{
	bool player, hierOk, losMasked, hierTerm, hierUnreach, hierInvalid, hierOther, instRefuse, plFalse;
	bool fallback, rescued, bothFail, keptTerm;
	bool npcRefuse, npcShadow, npcFalse, npcTrue, npcUndec;
	bool iterPair;             // an observe player search whose two arms both ran and whose
	                           // vanilla arm ran more than 1,000 iterations, at any status
	AstarHierEvent event;
};

AstarHierSubject AstarHierSubjectOf(bool atCharacterSite, unsigned char hierByte);
bool AstarHierInstantRefusal(const AstarHierResult& r);   // status 2, cause 0, iterations 1
bool AstarHierAstarSuccess(const AstarHierResult& r);     // status 1, iterations >= 1
bool AstarHierLosOnly(const AstarHierResult& r);          // status 1, iterations 0
AstarHierNext AstarHierAfterHier(int status, int iters, AstarHierOnCap onCap, bool costModifier);
bool AstarHierNpcSampleDue(unsigned long seq, long long nowTicks, long long lastTicks, long long minGapTicks);
AstarHierRefusal AstarHierClassifyRefusal(const AstarHierResult& vanilla);
void AstarHierResetOutput(unsigned char* findPathOutput);
void AstarHierReadOutput(const unsigned char* findPathOutput, AstarHierResult* out);
void AstarHierRun(AstarHierMode mode, AstarHierOnCap onCap, AstarHierSubject subject, bool costModifier,
                  const AstarHierOps& ops, AstarHierRecord* rec);
void AstarHierTally(AstarHierMode mode, const AstarHierRecord& rec, AstarHierBumps* out);
// hier.iters / van.iters on an iteration pair (an observe player search whose two arms both
// ran and whose vanilla arm ran more than 1,000 iterations, at any status); -1 otherwise.
float AstarHierIterRatio(AstarHierMode mode, const AstarHierRecord& rec);

const int AHIER_RATIO_BUCKETS = 48;       // [0.90 + 0.01k, 0.90 + 0.01(k+1)); 0 takes below, 47 above
const int AHIER_ITER_RATIO_BUCKETS = 21;  // [0.05k, 0.05(k+1)); 20 takes 1.00 and above
int AstarHierRatioBucket(float ratio);
int AstarHierIterRatioBucket(float ratio);
// The smallest bucket whose running total reaches pct percent of all, or -1 when empty.
int AstarHierPercentileBucket(const long* hist, int buckets, int pct);
const char* AstarHierModeName(int mode);    // "off", "observe", "on"
const char* AstarHierOnCapName(int onCap);  // "rerun", "keep"

#endif
