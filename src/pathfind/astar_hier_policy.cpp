// astar_hier_policy.cpp - the hierarchical arm's call sequence, output reset and counter
// buckets. Pure: any thread, no lock, no allocation; the game binds it through AstarHierOps.

#include "pathfind/astar_hier_policy.h"
#include <string.h>
#include <math.h>

AstarHierSubject AstarHierSubjectOf(bool atCharacterSite, unsigned char hierByte)
{
	if (!atCharacterSite)
		return AHIER_SUBJECT_OTHER;
	if (hierByte == 0)
		return AHIER_SUBJECT_PLAYER;
	if (hierByte == 1)
		return AHIER_SUBJECT_NPC;
	return AHIER_SUBJECT_OTHER;
}

bool AstarHierInstantRefusal(const AstarHierResult& r)
{
	return r.status == 2 && r.cause == 0 && r.iters == 1;
}

bool AstarHierAstarSuccess(const AstarHierResult& r)
{
	return r.status == 1 && r.iters >= 1;
}

bool AstarHierLosOnly(const AstarHierResult& r)
{
	return r.status == 1 && r.iters == 0;
}

// A line-of-sight answer with a cost modifier can hide a refusal, so it re-runs; without one
// both arms would return the same answer. A status 5 is an input vanilla refuses too.
AstarHierNext AstarHierAfterHier(int status, int iters, AstarHierOnCap onCap, bool costModifier)
{
	if (status == 1)
	{
		if (iters == 0)
			return costModifier ? AHIER_NEXT_RERUN : AHIER_NEXT_KEEP;
		return AHIER_NEXT_KEEP;
	}
	if (status == 3)
		return onCap == AHIER_CAP_KEEP ? AHIER_NEXT_KEEP : AHIER_NEXT_RERUN;
	if (status == 5)
		return AHIER_NEXT_KEEP;
	return AHIER_NEXT_RERUN;
}

bool AstarHierNpcSampleDue(unsigned long seq, long long nowTicks, long long lastTicks, long long minGapTicks)
{
	if (seq % 4 != 1)
		return false;
	return lastTicks == 0 || nowTicks - lastTicks >= minGapTicks;
}

AstarHierRefusal AstarHierClassifyRefusal(const AstarHierResult& vanilla)
{
	if (vanilla.status == 1 || vanilla.status == 3)
		return AHIER_REFUSAL_FALSE;
	if (vanilla.status == 2)
		return AHIER_REFUSAL_TRUE;
	return AHIER_REFUSAL_UNDECIDED;
}

// Only the scalars a second call can inherit; the buffer pointers and capacities stay, since
// the search grows those buffers and never frees them.
void AstarHierResetOutput(unsigned char* findPathOutput)
{
	unsigned char* o = findPathOutput;
	*(int*)(o + AHIER_OUT_VISITED_SIZE) = 0;
	*(int*)(o + AHIER_OUT_PATH_SIZE) = 0;
	*(int*)(o + AHIER_OUT_ITERATIONS) = 0;
	*(int*)(o + AHIER_OUT_GOAL_INDEX) = -1;
	*(unsigned*)(o + AHIER_OUT_COST) = AHIER_COST_UNSET;
	*(unsigned short*)(o + AHIER_OUT_STATUS) = 0;
}

void AstarHierReadOutput(const unsigned char* findPathOutput, AstarHierResult* out)
{
	const unsigned char* o = findPathOutput;
	out->status = o[AHIER_OUT_STATUS];
	out->cause  = o[AHIER_OUT_STATUS + 1];
	out->iters  = *(const int*)(o + AHIER_OUT_ITERATIONS);
	memcpy(&out->cost, o + AHIER_OUT_COST, sizeof(out->cost));
}

static void AhTimed(const AstarHierOps& ops, AstarHierRecord* rec, AstarHierResult* r, long long* ticks)
{
	long long t0 = ops.now(ops.ctx);
	ops.search(ops.ctx);
	*ticks += ops.now(ops.ctx) - t0;
	ops.readOutput(ops.ctx, r);
	rec->last = *r;
	++rec->calls;
}

static void AhVanillaArm(const AstarHierOps& ops, AstarHierRecord* rec)
{
	ops.resetOutput(ops.ctx);
	ops.setHierByte(ops.ctx, 0);
	AhTimed(ops, rec, &rec->van, &rec->vanTicks);
	rec->vanRan = true;
}

void AstarHierRun(AstarHierMode mode, AstarHierOnCap onCap, AstarHierSubject subject, bool costModifier,
                  const AstarHierOps& ops, AstarHierRecord* rec)
{
	memset(rec, 0, sizeof(*rec));
	rec->subject = subject;
	if (mode == AHIER_OFF || subject == AHIER_SUBJECT_OTHER)
	{
		ops.search(ops.ctx);
		rec->calls = 1;
		return;
	}
	if (subject == AHIER_SUBJECT_NPC)
	{
		AhTimed(ops, rec, &rec->hier, &rec->hierTicks);
		rec->hierRan = true;
		if (mode == AHIER_OBSERVE && AstarHierInstantRefusal(rec->hier) && ops.npcSampleDue(ops.ctx))
		{
			rec->npcShadow = true;
			AhVanillaArm(ops, rec);
			rec->npcClass = AstarHierClassifyRefusal(rec->van);
			ops.resetOutput(ops.ctx);
			ops.setHierByte(ops.ctx, 1);
			AstarHierResult again;
			AhTimed(ops, rec, &again, &rec->hierTicks);
		}
		return;
	}
	ops.setHierByte(ops.ctx, 1);
	AhTimed(ops, rec, &rec->hier, &rec->hierTicks);
	rec->hierRan = true;
	rec->losMasked = AstarHierLosOnly(rec->hier) && costModifier;
	if (mode == AHIER_OBSERVE)
	{
		if (!(AstarHierLosOnly(rec->hier) && !costModifier))
			AhVanillaArm(ops, rec);
	}
	else
	{
		rec->rerun = AstarHierAfterHier(rec->hier.status, rec->hier.iters, onCap, costModifier) == AHIER_NEXT_RERUN;
		rec->keptTerm = !rec->rerun && rec->hier.status == 3;
		if (rec->rerun)
			AhVanillaArm(ops, rec);
	}
	ops.setHierByte(ops.ctx, 0);
	rec->ratioValid = rec->vanRan && AstarHierAstarSuccess(rec->hier) && AstarHierAstarSuccess(rec->van)
	               && rec->van.cost > 0.0f;
	if (rec->ratioValid)
		rec->ratio = rec->hier.cost / rec->van.cost;
}

float AstarHierIterRatio(AstarHierMode mode, const AstarHierRecord& rec)
{
	if (mode != AHIER_OBSERVE || rec.subject != AHIER_SUBJECT_PLAYER || !rec.hierRan || !rec.vanRan
	    || rec.van.iters <= 1000)
		return -1.0f;
	return (float)rec.hier.iters / (float)rec.van.iters;
}

static void AhTallyPlayer(AstarHierMode mode, const AstarHierRecord& rec, AstarHierBumps* out)
{
	out->player = true;
	const int st = rec.hier.status;
	if (rec.losMasked)      out->losMasked = true;
	else if (st == 1)       out->hierOk = true;
	else if (st == 3)       out->hierTerm = true;
	else if (st == 2)       out->hierUnreach = true;
	else if (st == 5)       out->hierInvalid = true;
	else                    out->hierOther = true;
	out->instRefuse = AstarHierInstantRefusal(rec.hier);

	if (mode == AHIER_ON)
	{
		out->fallback = rec.rerun;
		out->rescued  = rec.rerun && rec.van.status == 1;
		out->bothFail = rec.rerun && rec.van.status != 1;
		out->keptTerm = rec.keptTerm;
		if (out->rescued)
			out->event = AHIER_EVENT_RESCUE;
		return;
	}

	out->plFalse  = out->instRefuse && rec.vanRan && (rec.van.status == 1 || rec.van.status == 3);
	out->iterPair = AstarHierIterRatio(mode, rec) >= 0.0f;
	if (out->plFalse)
		out->event = AHIER_EVENT_FALSE_REFUSAL;
	else if (rec.hierRan && rec.vanRan && rec.hier.status != rec.van.status)
		out->event = AHIER_EVENT_DIVERGE;
}

static void AhTallyNpc(const AstarHierRecord& rec, AstarHierBumps* out)
{
	out->npcRefuse = AstarHierInstantRefusal(rec.hier);
	out->npcShadow = rec.npcShadow;
	if (!rec.npcShadow)
		return;
	out->npcFalse = rec.npcClass == AHIER_REFUSAL_FALSE;
	out->npcTrue  = rec.npcClass == AHIER_REFUSAL_TRUE;
	out->npcUndec = rec.npcClass == AHIER_REFUSAL_UNDECIDED;
	if (out->npcFalse)
		out->event = AHIER_EVENT_NPC_FALSE;
}

void AstarHierTally(AstarHierMode mode, const AstarHierRecord& rec, AstarHierBumps* out)
{
	memset(out, 0, sizeof(*out));
	if (mode == AHIER_OFF)
		return;
	if (rec.subject == AHIER_SUBJECT_PLAYER)
		AhTallyPlayer(mode, rec, out);
	else if (rec.subject == AHIER_SUBJECT_NPC)
		AhTallyNpc(rec, out);
}

// The edges are compared in whole hundredths (twentieths for the iteration ratio), with a
// small allowance so that a float just under an edge, such as 0.95f, lands on the edge.
int AstarHierRatioBucket(float ratio)
{
	if (!(ratio >= 0.90f))
		return 0;
	double k = floor((double)ratio * 100.0 + 1e-4) - 90.0;
	if (k < 0.0)
		return 0;
	if (k > (double)(AHIER_RATIO_BUCKETS - 1))
		return AHIER_RATIO_BUCKETS - 1;
	return (int)k;
}

int AstarHierIterRatioBucket(float ratio)
{
	if (!(ratio >= 0.0f))
		return 0;
	double k = floor((double)ratio * 20.0 + 1e-4);
	if (k > (double)(AHIER_ITER_RATIO_BUCKETS - 1))
		return AHIER_ITER_RATIO_BUCKETS - 1;
	return (int)k;
}

int AstarHierPercentileBucket(const long* hist, int buckets, int pct)
{
	long long total = 0;
	for (int i = 0; i < buckets; ++i)
		total += hist[i];
	if (total <= 0)
		return -1;
	long long running = 0;
	for (int i = 0; i < buckets; ++i)
	{
		running += hist[i];
		if (running * 100 >= (long long)pct * total)
			return i;
	}
	return buckets - 1;
}

const char* AstarHierModeName(int mode)
{
	if (mode == AHIER_OBSERVE)
		return "observe";
	if (mode == AHIER_ON)
		return "on";
	return "off";
}

const char* AstarHierOnCapName(int onCap)
{
	return onCap == AHIER_CAP_KEEP ? "keep" : "rerun";
}
