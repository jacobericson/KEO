#include "navmesh/workers/nm_retire_policy.h"
#include <cstdio>
#include <cstring>

RetireAction RetireDecide(unsigned elapsedMs, int liveCount, bool waitFailed)
{
	// The live count comes first: a worker that exits at the cap still proceeds.
	if (liveCount <= 0)
		return RETIRE_PROCEED;
	if (elapsedMs >= RETIRE_CAP_MS)
		return RETIRE_TERMINATE;
	if (waitFailed || elapsedMs >= RETIRE_REPORT_MS)
		return RETIRE_REPORT_SLICE;
	return RETIRE_WAIT_SLICE;
}

unsigned RetireNextWaitMs(unsigned elapsedMs)
{
	if (elapsedMs >= RETIRE_CAP_MS)
		return 0;
	const unsigned left = RETIRE_CAP_MS - elapsedMs;
	return left < RETIRE_SLICE_MS ? left : RETIRE_SLICE_MS;
}

// The joined= value of a slice line or the final record.
static const char* RetireJoinedValue(RetireAction action, bool waitFailed)
{
	if (waitFailed)
		return "FAILED";
	return action == RETIRE_WAIT_SLICE ? "TIMEOUT" : "HANG";
}

// " gle=<n>" after a failed wait, else empty.
static void RetireGlePart(char* out, size_t cap, bool waitFailed, unsigned long gle)
{
	out[0] = 0;
	if (waitFailed)
		_snprintf_s(out, cap, _TRUNCATE, " gle=%lu", gle);
}

size_t RetireFormatSliceLine(char* out, size_t cap, const RetireLineInput& in)
{
	if (cap == 0)
		return 0;
	char gle[32];
	RetireGlePart(gle, sizeof(gle), in.waitFailed, in.gle);
	_snprintf_s(out, cap, _TRUNCATE,
		"NavMesh workers still live at NavMesh::stop: retireSlice=%d joined=%s waitMs=%u/%u%s live=%d stopDrop=%ld phases=[%s]",
		in.slice, RetireJoinedValue(in.action, in.waitFailed), in.elapsedMs, RETIRE_CAP_MS, gle,
		in.live, in.stopDrop, in.phases ? in.phases : "");
	return strlen(out);
}

size_t RetireFormatTerminateLine(char* out, size_t cap, const RetireLineInput& in)
{
	if (cap == 0)
		return 0;
	char gle[32];
	RetireGlePart(gle, sizeof(gle), in.waitFailed, in.gle);
	_snprintf_s(out, cap, _TRUNCATE,
		"RETIRE TERMINATE: exit code %u, retireSlice=%d joined=%s waitMs=%u/%u%s live=%d stopDrop=%ld phases=[%s]",
		RETIRE_EXIT_CODE, in.slice, RetireJoinedValue(in.action, in.waitFailed), in.elapsedMs, RETIRE_CAP_MS,
		gle, in.live, in.stopDrop, in.phases ? in.phases : "");
	return strlen(out);
}

size_t RetireFormatRetiredLine(char* out, size_t cap, const RetireSummary& s)
{
	if (cap == 0)
		return 0;
	const char* joined = "ok";
	if (s.anyWaitFailed)
		joined = "FAILED";
	else if (s.waitMs >= RETIRE_REPORT_MS)
		joined = "HANG";
	char gle[32];
	RetireGlePart(gle, sizeof(gle), s.anyWaitFailed, s.lastGle);
	char left[32];
	left[0] = 0;
	if (s.cleanupLeft > 0)
		_snprintf_s(left, sizeof(left), _TRUNCATE, " cleanupLeft=%ld", s.cleanupLeft);
	_snprintf_s(out, cap, _TRUNCATE,
		"NavMesh workers retired at NavMesh::stop: %d joined=%s waitMs=%u/%u%s live=%d stopDrop=%ld%s",
		s.activeCount, joined, s.waitMs, RETIRE_CAP_MS, gle, s.live, s.stopDrop, left);
	return strlen(out);
}

void RetireEmit(const RetireOps& ops, bool* logStuck, const char* line)
{
	if (!*logStuck)
	{
		if (ops.log(ops.ctx, line))
			return;
		*logStuck = true;
	}
	ops.logFallback(ops.ctx, line);
}

RetireResult RetireRun(const RetireOps& ops)
{
	RetireResult r;
	memset(&r, 0, sizeof(r));
	const unsigned t0 = ops.nowMs(ops.ctx);
	int done = 0;
	bool lastFailed = false;
	bool lastJoined = false;
	unsigned long lastGle = 0;
	for (;;)
	{
		const int live = ops.liveCount(ops.ctx);
		// A join the live count does not confirm still spent a slice: the loop cannot spin.
		if (lastJoined && live > 0)
			++done;
		const unsigned clock = ops.nowMs(ops.ctx) - t0;
		const unsigned floorMs = (unsigned)done * RETIRE_SLICE_MS;
		const unsigned elapsed = clock > floorMs ? clock : floorMs;
		const RetireAction a = RetireDecide(elapsed, live, lastFailed);
		r.slices = done;
		r.waitMs = clock;
		if (a == RETIRE_PROCEED)
			return r;
		if (done > 0 || a == RETIRE_TERMINATE)
		{
			char phases[RETIRE_PHASES_CHARS];
			phases[0] = 0;
			ops.phases(ops.ctx, phases, sizeof(phases));
			RetireLineInput in;
			in.action     = a;
			in.slice      = done;
			in.elapsedMs  = elapsed;
			in.live       = live;
			in.stopDrop   = ops.stopDrops(ops.ctx);
			in.waitFailed = lastFailed;
			in.gle        = lastGle;
			in.phases     = phases;
			char line[RETIRE_LINE_CHARS];
			if (a == RETIRE_TERMINATE)
				RetireFormatTerminateLine(line, sizeof(line), in);
			else
				RetireFormatSliceLine(line, sizeof(line), in);
			RetireEmit(ops, &r.logStuck, line);
			if (a == RETIRE_TERMINATE)
			{
				ops.terminate(ops.ctx, RETIRE_EXIT_CODE);
				r.terminated = true;
				return r;
			}
		}
		unsigned long gle = 0;
		const RetireWait w = ops.waitSlice(ops.ctx, RetireNextWaitMs(elapsed), &gle);
		lastFailed = (w == RETIRE_WAIT_FAILED);
		lastJoined = (w == RETIRE_WAIT_JOINED);
		if (lastFailed)
		{
			lastGle = gle;
			r.anyWaitFailed = true;
			r.lastGle = gle;
		}
		if (!lastJoined)
			++done;
	}
}
