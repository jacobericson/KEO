// The hierarchical arm's call sequence through fake operations that log every call: S<byte> a
// search with the byte then set, R a reset, B<v> a byte write, T a clock read, O an output read.
// Each sequence row compares the whole log, so a dropped or added call turns it red. Also the
// output reset's exact stores, the tally's buckets, the sampler and the histogram buckets.

#include <stdio.h>
#include <string.h>
#include "pathfind/astar_hier_policy.h"

#include "check.h"

struct Fake
{
	char log[256];
	int len;
	unsigned char byte;
	AstarHierResult queue[4];
	int queued;
	int next;
	AstarHierResult output;
	bool sampleDue;
	int sampleCalls;
	long long clock;
};

static void Put(Fake* f, char c, int v)
{
	if (f->len > 0 && f->len < (int)sizeof(f->log) - 1)
		f->log[f->len++] = ' ';
	if (f->len < (int)sizeof(f->log) - 1)
		f->log[f->len++] = c;
	if (v >= 0 && f->len < (int)sizeof(f->log) - 1)
		f->log[f->len++] = (char)('0' + v);
	f->log[f->len] = 0;
}

static void FakeSearch(void* ctx)
{
	Fake* f = (Fake*)ctx;
	Put(f, 'S', f->byte);
	if (f->next < f->queued)
		f->output = f->queue[f->next++];
}

static void FakeSetByte(void* ctx, unsigned char v)
{
	Fake* f = (Fake*)ctx;
	f->byte = v;
	Put(f, 'B', v);
}

static void FakeReset(void* ctx)
{
	Fake* f = (Fake*)ctx;
	f->output.status = 0;
	f->output.cause = 0;
	f->output.iters = 0;
	f->output.cost = -1.0f;
	Put(f, 'R', -1);
}

static void FakeRead(void* ctx, AstarHierResult* out)
{
	Fake* f = (Fake*)ctx;
	*out = f->output;
	Put(f, 'O', -1);
}

static long long FakeNow(void* ctx)
{
	Fake* f = (Fake*)ctx;
	Put(f, 'T', -1);
	f->clock += 10;
	return f->clock;
}

static bool FakeSampleDue(void* ctx)
{
	Fake* f = (Fake*)ctx;
	++f->sampleCalls;
	return f->sampleDue;
}

static AstarHierResult Res(int status, int cause, int iters, float cost)
{
	AstarHierResult r;
	r.status = status;
	r.cause = cause;
	r.iters = iters;
	r.cost = cost;
	return r;
}

static bool Same(const AstarHierResult& a, const AstarHierResult& b)
{
	return a.status == b.status && a.cause == b.cause && a.iters == b.iters && a.cost == b.cost;
}

static AstarHierOps Ops(Fake* f)
{
	AstarHierOps ops;
	ops.ctx = f;
	ops.search = FakeSearch;
	ops.setHierByte = FakeSetByte;
	ops.resetOutput = FakeReset;
	ops.readOutput = FakeRead;
	ops.now = FakeNow;
	ops.npcSampleDue = FakeSampleDue;
	return ops;
}

// One run: the byte the call site passed, then the results the searches return in order.
static void Run(Fake* f, AstarHierMode mode, AstarHierOnCap onCap, AstarHierSubject subject, bool costModifier,
                unsigned char byte, int n, const AstarHierResult* results, bool sampleDue, AstarHierRecord* rec)
{
	memset(f, 0, sizeof(*f));
	f->byte = byte;
	f->output = Res(9, 9, 9, 9.0f);
	for (int i = 0; i < n && i < 4; ++i)
		f->queue[i] = results[i];
	f->queued = n;
	f->sampleDue = sampleDue;
	AstarHierRun(mode, onCap, subject, costModifier, Ops(f), rec);
}

static bool LogIs(const Fake& f, const char* want)
{
	if (strcmp(f.log, want) == 0)
		return true;
	printf("  log \"%s\", expected \"%s\"\n", f.log, want);
	return false;
}

static const char* kHierThenVanilla = "B1 T S1 T O R B0 T S0 T O B0";
static const char* kHierOnly        = "B1 T S1 T O B0";

static void CheckOffAndOther()
{
	Fake f;
	AstarHierRecord rec;
	AstarHierResult r[2] = { Res(2, 0, 1, 0.0f), Res(1, 0, 50, 10.0f) };

	Run(&f, AHIER_OFF, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, r, true, &rec);
	bool ok = LogIs(f, "S0") && rec.calls == 1 && f.byte == 0 && f.sampleCalls == 0;
	Run(&f, AHIER_OFF, AHIER_CAP_KEEP, AHIER_SUBJECT_NPC, true, 1, 2, r, true, &rec);
	ok = ok && LogIs(f, "S1") && rec.calls == 1 && f.byte == 1 && f.sampleCalls == 0;
	Check(ok, "off: exactly one original call, no byte write, no reset");

	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_OTHER, false, 0, 2, r, true, &rec);
	ok = LogIs(f, "S0") && rec.calls == 1;
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_OTHER, true, 1, 2, r, true, &rec);
	ok = ok && LogIs(f, "S1") && rec.calls == 1 && f.sampleCalls == 0;
	Check(ok, "other: exactly one original call");
}

static void CheckSubject()
{
	Check(AstarHierSubjectOf(false, 0) == AHIER_SUBJECT_OTHER && AstarHierSubjectOf(false, 1) == AHIER_SUBJECT_OTHER
	      && AstarHierSubjectOf(true, 0) == AHIER_SUBJECT_PLAYER && AstarHierSubjectOf(true, 1) == AHIER_SUBJECT_NPC
	      && AstarHierSubjectOf(true, 2) == AHIER_SUBJECT_OTHER,
	      "subject: not at the character site is other, byte 0 is a player, byte 1 an npc");
}

static void CheckObservePlayer()
{
	Fake f;
	AstarHierRecord rec;

	AstarHierResult shadow[2] = { Res(1, 0, 500, 110.0f), Res(1, 0, 800, 100.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, shadow, false, &rec);
	Check(LogIs(f, kHierThenVanilla) && rec.calls == 2 && rec.hierRan && rec.vanRan
	      && Same(rec.hier, shadow[0]) && Same(rec.van, shadow[1]) && Same(rec.last, shadow[1])
	      && Same(f.output, shadow[1]) && f.byte == 0,
	      "observe player: hierarchical shadow, then vanilla returned");

	AstarHierResult los[2] = { Res(1, 0, 0, 40.0f), Res(1, 0, 60, 45.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, los, false, &rec);
	Check(LogIs(f, kHierOnly) && rec.calls == 1 && !rec.vanRan && !rec.losMasked && Same(f.output, los[0]),
	      "observe player: a direct line-of-sight answer skips the vanilla call");

	AstarHierBumps b;
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, true, 0, 2, los, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	Check(LogIs(f, kHierThenVanilla) && rec.calls == 2 && rec.losMasked && b.losMasked && !b.hierOk
	      && Same(f.output, los[1]),
	      "observe player: a masked line-of-sight answer runs vanilla and counts losMasked");

	AstarHierResult capped[2] = { Res(3, 3, 32768, 70.0f), Res(1, 0, 900, 120.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, capped, false, &rec);
	bool rerunLog = LogIs(f, kHierThenVanilla) && Same(f.output, capped[1]);
	Run(&f, AHIER_OBSERVE, AHIER_CAP_KEEP, AHIER_SUBJECT_PLAYER, false, 0, 2, capped, false, &rec);
	Check(rerunLog && LogIs(f, kHierThenVanilla) && Same(f.output, capped[1]) && !rec.keptTerm && !rec.rerun,
	      "observe player: onCap changes nothing");
}

static void CheckOnPlayer()
{
	Fake f;
	AstarHierRecord rec;
	AstarHierBumps b;

	AstarHierResult ok[2] = { Res(1, 0, 300, 90.0f), Res(1, 0, 700, 85.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, ok, false, &rec);
	Check(LogIs(f, kHierOnly) && rec.calls == 1 && !rec.rerun && Same(f.output, ok[0]) && Same(rec.last, ok[0]),
	      "on: a hierarchical success is kept");

	AstarHierResult refused[2] = { Res(2, 0, 1, 0.0f), Res(1, 0, 1200, 130.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, refused, false, &rec);
	Check(LogIs(f, kHierThenVanilla) && rec.calls == 2 && rec.rerun && Same(f.output, refused[1])
	      && Same(rec.last, refused[1]) && f.byte == 0,
	      "on: a hierarchical status 2 returns the vanilla re-run");

	Run(&f, AHIER_ON, AHIER_CAP_KEEP, AHIER_SUBJECT_PLAYER, false, 0, 2, refused, false, &rec);
	Check(LogIs(f, kHierThenVanilla) && rec.rerun && Same(f.output, refused[1]),
	      "on: status 2 re-runs under keep too");

	AstarHierResult capped[2] = { Res(3, 3, 32768, 70.0f), Res(3, 3, 32768, 75.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, capped, false, &rec);
	bool rerunOk = LogIs(f, kHierThenVanilla) && rec.rerun && !rec.keptTerm && Same(f.output, capped[1]);
	Run(&f, AHIER_ON, AHIER_CAP_KEEP, AHIER_SUBJECT_PLAYER, false, 0, 2, capped, false, &rec);
	AstarHierTally(AHIER_ON, rec, &b);
	Check(rerunOk && LogIs(f, kHierOnly) && !rec.rerun && rec.keptTerm && b.keptTerm && !b.fallback
	      && Same(f.output, capped[0]),
	      "on: status 3 re-runs under rerun and is kept under keep");

	AstarHierResult invalid[2] = { Res(5, 0, 0, 0.0f), Res(5, 0, 0, 0.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, invalid, false, &rec);
	Check(LogIs(f, kHierOnly) && !rec.rerun && Same(f.output, invalid[0]), "on: status 5 is kept");

	AstarHierResult los[2] = { Res(1, 0, 0, 40.0f), Res(1, 0, 60, 45.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, true, 0, 2, los, false, &rec);
	AstarHierTally(AHIER_ON, rec, &b);
	Check(LogIs(f, kHierThenVanilla) && rec.rerun && rec.losMasked && b.losMasked && b.fallback
	      && Same(f.output, los[1]),
	      "on: a masked line-of-sight answer re-runs vanilla and counts losMasked");

	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, los, false, &rec);
	Check(LogIs(f, kHierOnly) && !rec.rerun && !rec.losMasked && Same(f.output, los[0]),
	      "on: a direct line-of-sight answer is kept");
}

static void CheckNpc()
{
	Fake f;
	AstarHierRecord rec;
	AstarHierBumps b;

	AstarHierResult shadow[3] = { Res(2, 0, 1, 0.0f), Res(1, 0, 400, 50.0f), Res(2, 0, 1, 0.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_NPC, false, 1, 3, shadow, true, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	Check(LogIs(f, "T S1 T O R B0 T S0 T O R B1 T S1 T O") && rec.calls == 3 && rec.npcShadow
	      && rec.npcClass == AHIER_REFUSAL_FALSE && Same(rec.hier, shadow[0]) && Same(rec.van, shadow[1])
	      && Same(rec.last, shadow[2]) && Same(f.output, shadow[2]) && f.byte == 1 && f.sampleCalls == 1
	      && b.npcRefuse && b.npcShadow && b.npcFalse && b.event == AHIER_EVENT_NPC_FALSE,
	      "observe npc: a sampled instant refusal runs vanilla, then the hierarchical re-run last");

	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_NPC, false, 1, 3, shadow, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	Check(LogIs(f, "T S1 T O") && rec.calls == 1 && !rec.npcShadow && f.sampleCalls == 1
	      && Same(f.output, shadow[0]) && b.npcRefuse && !b.npcShadow,
	      "observe npc: an unsampled refusal makes one call");

	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_NPC, false, 1, 3, shadow, true, &rec);
	Check(LogIs(f, "T S1 T O") && rec.calls == 1 && f.sampleCalls == 0 && Same(f.output, shadow[0])
	      && f.byte == 1,
	      "on npc: one call");
}

// Every mode, cap, subject and cost modifier over first results of every status.
static void CheckEverySequence()
{
	const AstarHierMode modes[3] = { AHIER_OFF, AHIER_OBSERVE, AHIER_ON };
	const AstarHierOnCap caps[2] = { AHIER_CAP_RERUN, AHIER_CAP_KEEP };
	const AstarHierSubject subjects[3] = { AHIER_SUBJECT_OTHER, AHIER_SUBJECT_PLAYER, AHIER_SUBJECT_NPC };
	const AstarHierResult firsts[8] = { Res(0, 0, 0, 0.0f), Res(1, 0, 0, 5.0f), Res(1, 0, 20, 9.0f),
	                                    Res(2, 0, 1, 0.0f), Res(2, 3, 900, 0.0f), Res(3, 3, 32768, 60.0f),
	                                    Res(4, 0, 10, 0.0f), Res(5, 0, 0, 0.0f) };
	bool byteOk = true, resetOk = true, returnOk = true;
	for (int m = 0; m < 3; ++m)
	for (int c = 0; c < 2; ++c)
	for (int s = 0; s < 3; ++s)
	for (int k = 0; k < 8; ++k)
	for (int mod = 0; mod < 2; ++mod)
	for (int due = 0; due < 2; ++due)
	{
		Fake f;
		AstarHierRecord rec;
		AstarHierResult r[3] = { firsts[k], Res(1, 0, 1500, 30.0f), firsts[k] };
		unsigned char byte = (subjects[s] == AHIER_SUBJECT_NPC) ? 1 : 0;
		Run(&f, modes[m], caps[c], subjects[s], mod != 0, byte, 3, r, due != 0, &rec);
		if (f.byte != byte)
			byteOk = false;
		const char* firstS = strchr(f.log, 'S');
		const char* firstR = strchr(f.log, 'R');
		if (!firstS || (firstR && firstR < firstS))
			resetOk = false;
		if (rec.calls > 1 && !Same(rec.last, f.output))
			returnOk = false;
	}
	Check(byteOk, "every sequence restores the byte it found");
	Check(resetOk, "no reset before the first search");
	Check(returnOk, "every sequence leaves the last call's output as the result");
}

static void CheckReset()
{
	unsigned char buf[64];
	memset(buf, 0xAB, sizeof(buf));
	AstarHierResetOutput(buf);

	bool ok = *(int*)(buf + 0x18) == 0 && *(int*)(buf + 0x28) == 0 && *(int*)(buf + 0x30) == 0
	       && *(int*)(buf + 0x34) == -1 && *(unsigned*)(buf + 0x38) == 0x7F7FFFEEu
	       && buf[0x3C] == 0 && buf[0x3D] == 0;
	for (int i = 0; i < 64; ++i)
	{
		bool stored = (i >= 0x18 && i < 0x1C) || (i >= 0x28 && i < 0x2C) || (i >= 0x30 && i < 0x3E);
		if (!stored && buf[i] != 0xAB)
			ok = false;
	}
	Check(ok, "reset: exact stores, buffer words untouched");

	unsigned char out[64];
	memset(out, 0, sizeof(out));
	float cost = 123.5f;
	out[0x3C] = 3;
	out[0x3D] = 2;
	*(int*)(out + 0x30) = 4567;
	memcpy(out + 0x38, &cost, sizeof(cost));
	AstarHierResult r;
	AstarHierReadOutput(out, &r);
	Check(r.status == 3 && r.cause == 2 && r.iters == 4567 && r.cost == 123.5f,
	      "read: status, cause, iterations and cost at their offsets");
}

static void CheckRatio()
{
	Fake f;
	AstarHierRecord rec;
	bool ok = true;

	AstarHierResult both[2] = { Res(1, 0, 500, 110.0f), Res(1, 0, 800, 100.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, both, false, &rec);
	ok = ok && rec.ratioValid && rec.ratio > 1.0999f && rec.ratio < 1.1001f;

	AstarHierResult masked[2] = { Res(1, 0, 0, 40.0f), Res(1, 0, 60, 45.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, true, 0, 2, masked, false, &rec);
	ok = ok && !rec.ratioValid;

	AstarHierResult vanCapped[2] = { Res(1, 0, 500, 110.0f), Res(3, 3, 32768, 100.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, vanCapped, false, &rec);
	ok = ok && !rec.ratioValid;

	AstarHierResult zeroCost[2] = { Res(1, 0, 500, 110.0f), Res(1, 0, 800, 0.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, zeroCost, false, &rec);
	ok = ok && !rec.ratioValid;

	AstarHierResult refused[2] = { Res(2, 0, 1, 0.0f), Res(1, 0, 800, 100.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, refused, false, &rec);
	ok = ok && !rec.ratioValid;

	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, both, false, &rec);
	ok = ok && !rec.ratioValid;
	Check(ok, "ratio only when both arms are A* successes");
}

static void CheckSampler()
{
	bool ok = true;
	for (unsigned long seq = 1; seq <= 8; ++seq)
		ok = ok && AstarHierNpcSampleDue(seq, 5000, 0, 1000) == (seq == 1 || seq == 5);
	ok = ok && !AstarHierNpcSampleDue(5, 1999, 1000, 1000);
	ok = ok && AstarHierNpcSampleDue(5, 2000, 1000, 1000);
	ok = ok && AstarHierNpcSampleDue(9, 9000, 1000, 1000);
	ok = ok && !AstarHierNpcSampleDue(6, 9000, 1000, 1000);
	Check(ok, "sampler: one in four, 100 ms apart");
}

static int OutcomeCount(const AstarHierBumps& b)
{
	return (b.hierOk ? 1 : 0) + (b.losMasked ? 1 : 0) + (b.hierTerm ? 1 : 0) + (b.hierUnreach ? 1 : 0)
	     + (b.hierInvalid ? 1 : 0) + (b.hierOther ? 1 : 0);
}

static void CheckTally()
{
	const AstarHierResult firsts[9] = { Res(0, 0, 0, 0.0f), Res(1, 0, 0, 5.0f), Res(1, 0, 20, 9.0f),
	                                    Res(2, 0, 1, 0.0f), Res(2, 3, 900, 0.0f), Res(3, 3, 32768, 60.0f),
	                                    Res(4, 0, 10, 0.0f), Res(5, 0, 0, 0.0f), Res(6, 0, 3, 0.0f) };
	bool oneBucket = true;
	long fallback = 0, unreach = 0, term = 0, kept = 0, masked = 0, other = 0;
	for (int mode = AHIER_OBSERVE; mode <= AHIER_ON; ++mode)
	for (int c = 0; c < 2; ++c)
	for (int k = 0; k < 9; ++k)
	for (int mod = 0; mod < 2; ++mod)
	{
		Fake f;
		AstarHierRecord rec;
		AstarHierBumps b;
		AstarHierResult r[2] = { firsts[k], Res(2, 3, 5000, 0.0f) };
		Run(&f, (AstarHierMode)mode, (AstarHierOnCap)c, AHIER_SUBJECT_PLAYER, mod != 0, 0, 2, r, false, &rec);
		AstarHierTally((AstarHierMode)mode, rec, &b);
		if (!b.player || OutcomeCount(b) != 1)
			oneBucket = false;
		if (mode == AHIER_ON)
		{
			fallback += b.fallback ? 1 : 0;
			unreach  += b.hierUnreach ? 1 : 0;
			term     += b.hierTerm ? 1 : 0;
			kept     += b.keptTerm ? 1 : 0;
			masked   += b.losMasked ? 1 : 0;
			other    += b.hierOther ? 1 : 0;
		}
	}
	Check(oneBucket, "tally: every player search in exactly one outcome bucket");
	Check(fallback > 0 && fallback == unreach + (term - kept) + masked + other,
	      "tally: on's fallback equals unreach plus rerun term plus losMasked plus other");

	Fake f;
	AstarHierRecord rec;
	AstarHierBumps b;
	AstarHierResult falseRefusal[2] = { Res(2, 0, 1, 0.0f), Res(3, 3, 32768, 80.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, falseRefusal, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	bool ok = b.instRefuse && b.plFalse && b.hierUnreach && b.event == AHIER_EVENT_FALSE_REFUSAL;
	AstarHierResult diverge[2] = { Res(3, 3, 32768, 80.0f), Res(1, 0, 4000, 90.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, diverge, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	ok = ok && !b.plFalse && b.event == AHIER_EVENT_DIVERGE && !b.fallback;
	AstarHierResult rescue[2] = { Res(2, 3, 900, 0.0f), Res(1, 0, 4000, 90.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, rescue, false, &rec);
	AstarHierTally(AHIER_ON, rec, &b);
	ok = ok && b.fallback && b.rescued && !b.bothFail && b.event == AHIER_EVENT_RESCUE && !b.plFalse;
	AstarHierResult bothFail[2] = { Res(2, 3, 900, 0.0f), Res(3, 3, 32768, 90.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, bothFail, false, &rec);
	AstarHierTally(AHIER_ON, rec, &b);
	ok = ok && b.fallback && !b.rescued && b.bothFail && b.event == AHIER_EVENT_NONE;
	AstarHierTally(AHIER_OFF, rec, &b);
	ok = ok && !b.player && !b.fallback && b.event == AHIER_EVENT_NONE;
	Check(ok, "tally: false refusal, divergence, rescue and both-fail events");
}

static void CheckIterRatio()
{
	Fake f;
	AstarHierRecord rec;
	AstarHierBumps b;

	AstarHierResult pair[2] = { Res(1, 0, 900, 70.0f), Res(3, 3, 32768, 60.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, pair, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	float q = AstarHierIterRatio(AHIER_OBSERVE, rec);
	Check(b.iterPair && q == (float)900 / (float)32768,
	      "iter ratio: a vanilla status 3 over 1,000 iterations is a pair");

	bool ok = true;
	AstarHierResult small[2] = { Res(1, 0, 400, 70.0f), Res(1, 0, 1000, 60.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, small, false, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	ok = ok && !b.iterPair && AstarHierIterRatio(AHIER_OBSERVE, rec) < 0.0f;

	AstarHierResult npc[3] = { Res(2, 0, 1, 0.0f), Res(1, 0, 5000, 50.0f), Res(2, 0, 1, 0.0f) };
	Run(&f, AHIER_OBSERVE, AHIER_CAP_RERUN, AHIER_SUBJECT_NPC, false, 1, 3, npc, true, &rec);
	AstarHierTally(AHIER_OBSERVE, rec, &b);
	ok = ok && rec.vanRan && !b.iterPair && AstarHierIterRatio(AHIER_OBSERVE, rec) < 0.0f;

	AstarHierResult rerun[2] = { Res(2, 3, 900, 0.0f), Res(1, 0, 5000, 50.0f) };
	Run(&f, AHIER_ON, AHIER_CAP_RERUN, AHIER_SUBJECT_PLAYER, false, 0, 2, rerun, false, &rec);
	AstarHierTally(AHIER_ON, rec, &b);
	ok = ok && rec.vanRan && !b.iterPair && AstarHierIterRatio(AHIER_ON, rec) < 0.0f;
	Check(ok, "iter ratio: no pair at or under 1,000 vanilla iterations, for an npc shadow or for an on re-run");
}

static void CheckPercentile()
{
	const long hist[4] = { 0, 2, 3, 5 };
	const long empty[4] = { 0, 0, 0, 0 };
	Check(AstarHierPercentileBucket(hist, 4, 50) == 2 && AstarHierPercentileBucket(hist, 4, 90) == 3
	      && AstarHierPercentileBucket(hist, 4, 20) == 1 && AstarHierPercentileBucket(empty, 4, 50) == -1,
	      "percentile: p50 and p90 of a known histogram");
}

static void CheckBuckets()
{
	Check(AstarHierRatioBucket(0.5f) == 0 && AstarHierRatioBucket(0.90f) == 0 && AstarHierRatioBucket(0.9099f) == 0
	      && AstarHierRatioBucket(0.91f) == 1 && AstarHierRatioBucket(0.95f) == 5 && AstarHierRatioBucket(1.00f) == 10
	      && AstarHierRatioBucket(1.3699f) == 46 && AstarHierRatioBucket(1.37f) == 47
	      && AstarHierRatioBucket(3.0f) == 47 && AstarHierRatioBucket(-1.0f) == 0,
	      "ratio bucket: below 0.90 in 0, each hundredth from 0.90, 1.37 and above in 47");
	Check(AstarHierIterRatioBucket(0.0f) == 0 && AstarHierIterRatioBucket(0.0499f) == 0
	      && AstarHierIterRatioBucket(0.05f) == 1 && AstarHierIterRatioBucket(0.5f) == 10
	      && AstarHierIterRatioBucket(0.95f) == 19 && AstarHierIterRatioBucket(0.9999f) == 19
	      && AstarHierIterRatioBucket(1.00f) == 20 && AstarHierIterRatioBucket(7.0f) == 20
	      && AstarHierIterRatioBucket(-1.0f) == 0,
	      "iter ratio bucket: each 0.05 from 0, 1.00 and above in 20");
}

static void CheckNames()
{
	Check(strcmp(AstarHierModeName(AHIER_OFF), "off") == 0 && strcmp(AstarHierModeName(AHIER_OBSERVE), "observe") == 0
	      && strcmp(AstarHierModeName(AHIER_ON), "on") == 0 && strcmp(AstarHierOnCapName(AHIER_CAP_RERUN), "rerun") == 0
	      && strcmp(AstarHierOnCapName(AHIER_CAP_KEEP), "keep") == 0,
	      "names: the mode and the cap");
}

int main()
{
	CheckOffAndOther();
	CheckSubject();
	CheckObservePlayer();
	CheckOnPlayer();
	CheckNpc();
	CheckEverySequence();
	CheckReset();
	CheckRatio();
	CheckSampler();
	CheckTally();
	CheckIterRatio();
	CheckPercentile();
	CheckBuckets();
	CheckNames();
	return CheckExit("astar_hier_policy_units");
}
