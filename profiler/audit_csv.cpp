// audit_csv.cpp - Frame and physics CSV output.
// Reporter thread; owns the output files and takes no locks.

#include "audit_detail.h"
#include <math.h>

namespace kenshiframeaudit_detail {
void WriteSlow(const FrameRec& r)
{
	std::string s = Fmt("[SLOW] t=%.3f seq=%u cls=%s flags=0x%X",
	                    r.t, r.seq, CLASS_NAMES[ClassOf(r)], r.flags);
	for (int m = 0; m < NUM_METRICS; ++m)
	{
		if (IsNan(r.m[m]))
			continue;
		s += Fmt(" %s=%s", METRIC_NAMES[m], V(r.m[m], 2).c_str());
	}
	for (int c = 0; c < NUM_COUNTS; ++c)
		s += Fmt(" %s=%d", COUNT_NAMES[c], r.c[c]);
	AuditOut(s);
}

void WriteFrameCsv(const FrameRec& r)
{
	if (!g_frameCsv)
		return;
	static bool header = false;
	if (!header)
	{
		std::string h = "t,seq,flags,class";
		for (int m = 0; m < NUM_METRICS; ++m) { h += ","; h += METRIC_NAMES[m]; }
		for (int c = 0; c < NUM_COUNTS; ++c)  { h += ","; h += COUNT_NAMES[c]; }
		WriteRaw(g_frameCsv, h);
		header = true;
	}
	std::string row = Fmt("%.4f,%u,%u,%s", r.t, r.seq, r.flags, CLASS_NAMES[ClassOf(r)]);
	for (int m = 0; m < NUM_METRICS; ++m)
		row += IsNan(r.m[m]) ? std::string(",") : Fmt(",%.3f", r.m[m]);
	for (int c = 0; c < NUM_COUNTS; ++c)
		row += Fmt(",%d", r.c[c]);
	WriteRaw(g_frameCsv, row);
}

void WritePhysCsv(const FrameRec& r)
{
	static bool physHeader = false, queryHeader = false;
	if (g_physCsv && r.phys.valid)
	{
		if (!physHeader)
		{
			WriteRaw(g_physCsv,
				"t,frame,runSeq,runMs,otherMs,lockMs,preMs,preOtherMs,makeMs,groupMs,"
				"impulseMs,hullDestroyMs,actorDestroyMs,terrainMs,hullApplyMs,simulateMs,"
				"flushMs,fetchMs,controllerMs,postMs,hulls,qMake,qGroup,qImpulse,"
				"qHullDestroy,qActorDestroy,qTerrain,nMake,nGroup,nImpulse,nHullDestroy,"
				"nActorDestroy,nTerrain,nHullApply");
			physHeader = true;
		}
		const PhysRunSample& p = r.phys;
		std::string row = Fmt(
			"%.4f,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
			"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
			"%d,%d,%d,%d,%d",
			r.t, r.seq, p.runSeq, p.runMs, p.phaseMs[PP_BODY_OTHER], p.lockMs, p.preMs,
			p.phaseMs[PP_PRE_OTHER], p.phaseMs[PP_MAKE], p.phaseMs[PP_GROUP],
			p.phaseMs[PP_IMPULSE], p.phaseMs[PP_HULL_DESTROY],
			p.phaseMs[PP_ACTOR_DESTROY], p.phaseMs[PP_TERRAIN],
			p.phaseMs[PP_HULL_APPLY], p.phaseMs[PP_SIMULATE], p.phaseMs[PP_FLUSH],
			p.phaseMs[PP_FETCH], p.phaseMs[PP_CONTROLLER], p.phaseMs[PP_POST], p.hulls,
			p.queued[PO_MAKE], p.queued[PO_GROUP], p.queued[PO_IMPULSE],
			p.queued[PO_HULL_DESTROY], p.queued[PO_ACTOR_DESTROY],
			p.queued[PO_TERRAIN], p.calls[PO_MAKE], p.calls[PO_GROUP],
			p.calls[PO_IMPULSE], p.calls[PO_HULL_DESTROY],
			p.calls[PO_ACTOR_DESTROY], p.calls[PO_TERRAIN], p.calls[PO_HULL_APPLY]);
		WriteRaw(g_physCsv, row);
	}
	if (g_physqCsv && r.nphysq > 0)
	{
		if (!queryHeader)
		{
			WriteRaw(g_physqCsv,
				"t,frame,ordinal,kind,ms,physRunning,runSeq,phase,owner,ownerTid,"
				"lockValid,lockCount,recursion");
			queryHeader = true;
		}
		for (int i = 0; i < r.nphysq; ++i)
		{
			const PhysQuerySample& q = r.physq[i];
			const char* kind  = q.kind >= 0 && q.kind < PQ_KIND_COUNT ? PHYS_QUERY_NAMES[q.kind] : "unknown";
			const char* phase = q.phase >= 0 && q.phase < PP_COUNT ? PHYS_PHASE_NAMES[q.phase] : "unknown";
			const char* owner = q.owner >= 0 && q.owner < POW_COUNT ? PHYS_OWNER_NAMES[q.owner] : "unknown";
			WriteRaw(g_physqCsv, Fmt("%.6f,%u,%d,%s,%.3f,%d,%d,%s,%s,%u,%d,%d,%d",
				q.t, r.seq, q.ordinal, kind, q.ms, q.physRunning, q.runSeq,
				phase, owner, q.ownerTid, q.lockValid, q.lockCount, q.recursion));
		}
	}
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
bool      g_secHeader = false;

void ResetSecond(long long second)
{
	g_sec.second   = second;
	g_sec.frames   = 0;
	g_sec.excluded = 0;
	g_sec.cn       = 0;
	g_sec.frameMs.clear();
	for (int i = 0; i < NUM_CLASSES; ++i) g_sec.cls[i] = 0;
	for (int m = 0; m < NUM_METRICS; ++m) { g_sec.sum[m] = 0.0; g_sec.mx[m] = Nan(); g_sec.n[m] = 0; }
	for (int c = 0; c < NUM_COUNTS; ++c)  { g_sec.csum[c] = 0.0; g_sec.cor[c] = 0; }
}

void WriteSecondRow()
{
	if (!g_secCsv || g_sec.second < 0 || g_sec.frames == 0)
		return;
	if (!g_secHeader)
	{
		std::string h = "t,frames,steady,stream,paused,menu,excluded,frame_mean,frame_p50,frame_p99,frame_max";
		for (int m = 0; m < NUM_METRICS; ++m)
		{
			if (m == M_FRAME) continue;
			h += Fmt(",%s_mean,%s_max", METRIC_NAMES[m], METRIC_NAMES[m]);
		}
		for (int c = 0; c < NUM_COUNTS; ++c)
			h += Fmt(",%s_%s", COUNT_NAMES[c], CountIsEvent(c) ? "sum" : (CountIsBits(c) ? "or" : "mean"));
		WriteRaw(g_secCsv, h);
		g_secHeader = true;
	}

	std::string row = Fmt("%lld,%d,%d,%d,%d,%d,%d", g_sec.second, g_sec.frames,
	                      g_sec.cls[CLS_STEADY], g_sec.cls[CLS_STREAM],
	                      g_sec.cls[CLS_PAUSED], g_sec.cls[CLS_MENU], g_sec.excluded);
	if (g_sec.n[M_FRAME] > 0)
	{
		float p50 = Percentile(g_sec.frameMs, 0.50);
		float p99 = Percentile(g_sec.frameMs, 0.99);
		row += Fmt(",%.3f,%.3f,%.3f,%.3f", g_sec.sum[M_FRAME] / g_sec.n[M_FRAME], p50, p99, g_sec.mx[M_FRAME]);
	}
	else
		row += ",,,,";
	for (int m = 0; m < NUM_METRICS; ++m)
	{
		if (m == M_FRAME) continue;
		if (g_sec.n[m] > 0)
			row += Fmt(",%.3f,%.3f", g_sec.sum[m] / g_sec.n[m], g_sec.mx[m]);
		else
			row += ",,";
	}
	for (int c = 0; c < NUM_COUNTS; ++c)
	{
		if (CountIsEvent(c))
			row += Fmt(",%.0f", g_sec.csum[c]);
		else if (CountIsBits(c))
			row += Fmt(",%u", g_sec.cor[c]);
		else
			row += g_sec.cn > 0 ? Fmt(",%.2f", g_sec.csum[c] / g_sec.cn) : std::string(",");
	}
	WriteRaw(g_secCsv, row);
}

void AccumulateSecond(const FrameRec& r)
{
	long long second = (long long)floor(r.t);
	if (g_sec.second != second)
	{
		WriteSecondRow();
		ResetSecond(second);
	}
	++g_sec.frames;
	++g_sec.cls[ClassOf(r)];
	if (Excluded(r)) { ++g_sec.excluded; return; }

	int cls = ClassOf(r);
	bool running = (cls == CLS_STEADY || cls == CLS_STREAM);
	if (!running)
		return;
	for (int m = 0; m < NUM_METRICS; ++m)
	{
		float v = r.m[m];
		if (IsNan(v)) continue;
		g_sec.sum[m] += v;
		if (g_sec.n[m] == 0 || v > g_sec.mx[m]) g_sec.mx[m] = v;
		++g_sec.n[m];
	}
	if (!IsNan(r.m[M_FRAME]))
		g_sec.frameMs.push_back(r.m[M_FRAME]);
	for (int c = 0; c < NUM_COUNTS; ++c)
	{
		g_sec.csum[c] += r.c[c];
		if (CountIsBits(c))
			g_sec.cor[c] |= (unsigned)r.c[c];
	}
	++g_sec.cn;
}

void OnRecord(const FrameRec& r)
{
	if (g_windowStart < 0.0)
		g_windowStart = r.t;
	g_window.push_back(r);

	// Split frames carry the audit's own shadow rays: no [SLOW] line.
	if ((r.flags & F_INGAME) && !(r.flags & (F_PAUSED | F_CURSORSPLIT)) &&
	    !IsNan(r.m[M_FRAME]) && r.m[M_FRAME] > g_cfg.slowMs)
	{
		if (g_slowThisWindow < g_cfg.slowMax) { WriteSlow(r); ++g_slowThisWindow; }
		else ++g_slowSuppressed;
	}
	if (g_cfg.csvFrames)
		WriteFrameCsv(r);
	if (g_cfg.physxDetail)
		WritePhysCsv(r);
	if (g_cfg.csvSeconds)
		AccumulateSecond(r);
	int cls = ClassOf(r);
	if ((cls == CLS_STEADY || cls == CLS_STREAM) && !Excluded(r) && r.ncalls > 0)
		AccumulateBuckets(r);

	if (r.t - g_windowStart >= g_cfg.summarySec || g_window.size() >= 4096)
		WriteSummary();
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
