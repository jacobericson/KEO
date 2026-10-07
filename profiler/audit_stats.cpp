// audit_stats.cpp - Frame statistics and summaries.
// Reporter thread; reads published records and counters without taking locks.

#include "audit_detail.h"
#include <algorithm>
#include <math.h>

namespace kenshiframeaudit_detail {
// =========================================================================
// Reporter thread: statistics
// =========================================================================

struct Stat
{
	int    n;
	float  p50, p90, p99, mx;
	double mean;
};

float Percentile(std::vector<float>& v, double p)
{
	size_t n = v.size();
	size_t k = (size_t)ceil(p * (double)n);
	if (k == 0) k = 1;
	if (k > n)  k = n;
	--k;
	std::nth_element(v.begin(), v.begin() + k, v.end());
	return v[k];
}

Stat ComputeStat(const std::vector<const FrameRec*>& frames, int metric, std::vector<float>& scratch)
{
	Stat s;
	s.n = 0;
	s.p50 = s.p90 = s.p99 = s.mx = Nan();
	s.mean = 0.0;
	scratch.clear();
	double sum = 0.0;
	for (size_t i = 0; i < frames.size(); ++i)
	{
		float v = frames[i]->m[metric];
		if (IsNan(v))
			continue;
		scratch.push_back(v);
		sum += v;
	}
	if (scratch.empty())
		return s;
	s.n    = (int)scratch.size();
	s.mean = sum / s.n;
	s.mx   = *std::max_element(scratch.begin(), scratch.end());
	s.p50  = Percentile(scratch, 0.50);
	s.p90  = Percentile(scratch, 0.90);
	s.p99  = Percentile(scratch, 0.99);
	return s;
}

double MeanCount(const std::vector<const FrameRec*>& frames, int count)
{
	if (frames.empty())
		return 0.0;
	double sum = 0.0;
	for (size_t i = 0; i < frames.size(); ++i)
		sum += frames[i]->c[count];
	return sum / frames.size();
}

double SumCount(const std::vector<const FrameRec*>& frames, int count)
{
	double sum = 0.0;
	for (size_t i = 0; i < frames.size(); ++i)
		sum += frames[i]->c[count];
	return sum;
}

double FlagShare(const std::vector<const FrameRec*>& frames, unsigned flag)
{
	if (frames.empty())
		return 0.0;
	int hits = 0;
	for (size_t i = 0; i < frames.size(); ++i)
		if (frames[i]->flags & flag) ++hits;
	return 100.0 * hits / frames.size();
}

// "name p50/p90" pairs for a metric range.
std::string PairList(const std::vector<const FrameRec*>& frames, int first, int last,
                     std::vector<float>& scratch)
{
	std::string s;
	for (int m = first; m <= last; ++m)
	{
		Stat st = ComputeStat(frames, m, scratch);
		s += Fmt(" %s %s/%s", METRIC_NAMES[m], V(st.p50, 2).c_str(), V(st.p90, 2).c_str());
	}
	return s;
}

// =========================================================================
// Reporter thread: summaries, [SLOW] lines, CSV
// =========================================================================

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
double g_windowStart     = -1.0;
int    g_slowThisWindow  = 0;
int    g_slowSuppressed  = 0;

void WriteClassSummary(const char* cls, const std::vector<const FrameRec*>& fr)
{
	std::vector<float> sc;
	Stat frame = ComputeStat(fr, M_FRAME, sc);
	int over16 = 0, over33 = 0;
	for (size_t i = 0; i < fr.size(); ++i)
	{
		float v = fr[i]->m[M_FRAME];
		if (!IsNan(v) && v > 16.667f) ++over16;
		if (!IsNan(v) && v > 33.333f) ++over33;
	}
	double n = fr.empty() ? 1.0 : (double)fr.size();
	AuditOut(Fmt("[AUDIT] %s n=%d frame p50/p90/p99/max %s/%s/%s/%s over16=%.1f%% over33=%.1f%%",
	             cls, (int)fr.size(), V(frame.p50, 2).c_str(), V(frame.p90, 2).c_str(),
	             V(frame.p99, 2).c_str(), V(frame.mx, 2).c_str(),
	             100.0 * over16 / n, 100.0 * over33 / n));

	AuditOut(std::string("[AUDIT] ") + cls + " top p50/p90" + PairList(fr, M_PRE, M_GAP, sc));
	AuditOut(std::string("[AUDIT] ") + cls + " ml p50/p90" + PairList(fr, M_ML_AIJOIN, M_ML_OTHER, sc));
	AuditOut(std::string("[AUDIT] ") + cls + " sub p50/p90" + PairList(fr, M_SUB_MOUSESCAN, M_SUB_ZCSECT, sc));
	if (g_cfg.steadyDetail)
		AuditOut(std::string("[AUDIT] ") + cls + " detail p50/p90" + PairList(fr, M_SD_CU, M_SD_LZCHAR, sc));
	AuditOut(std::string("[AUDIT] ") + cls + " render p50/p90" + PairList(fr, M_R_CULL, M_R_PASSOTHER, sc));
	AuditOut(std::string("[AUDIT] ") + cls + " thr p50/p90" + PairList(fr, M_AI_WAKE, M_PHYS_POST, sc) +
	         Fmt(" | aiJoin=%.0f%% birdsJoin=%.0f%% physRan=%.0f%% aiMiss=%.0f%% aiSync=%.0f%%",
	             FlagShare(fr, F_AIJOIN), FlagShare(fr, F_BIRDSJOIN), FlagShare(fr, F_PHYSRAN),
	             FlagShare(fr, F_AIMISS), FlagShare(fr, F_AISYNC)));
	if (g_cfg.particles)
		AuditOut(std::string("[AUDIT] ") + cls + " fx p50/p90" + PairList(fr, M_FX_UPD, M_FX_CENSUS, sc));

	Stat speed  = ComputeStat(fr, M_SPEED, sc);
	Stat dt     = ComputeStat(fr, M_DT, sc);
	Stat camAlt = ComputeStat(fr, M_CAM_ALT, sc);
	Stat zoneSM = ComputeStat(fr, M_ZONE_SM, sc);
	Stat unload = ComputeStat(fr, M_UNLOAD_MS, sc);
	Stat chars  = ComputeStat(fr, M_ML_CHARS, sc);
	Stat passes = ComputeStat(fr, M_PASSES, sc);
	double full  = MeanCount(fr, C_FULLRATE);
	double draws = MeanCount(fr, C_DRAWS);
	std::string usFull = (chars.n && full > 0.5)  ? Fmt("%.2f", chars.mean * 1000.0 / full)   : "-";
	std::string usDraw = (passes.n && draws > 0.5) ? Fmt("%.3f", passes.mean * 1000.0 / draws) : "-";

	std::string w = std::string("[AUDIT] ") + cls + " world mean";
	w += Fmt(" speed %s dt %s", V((float)speed.mean, 1).c_str(), V((float)dt.mean, 2).c_str());
	for (int c = 0; c < NUM_COUNTS; ++c)
	{
		if (CountIsEvent(c) || CountIsBits(c))
			continue;
		w += Fmt(" %s %.1f", COUNT_NAMES[c], MeanCount(fr, c));
	}
	w += Fmt(" camAlt %s | sum unloads %.0f squads %.0f fxNew %.0f fxDel %.0f | zoneSM p50/p90 %s/%s unloadMs max %s | usPerFull=%s usPerDraw=%s",
	         V((float)camAlt.mean, 0).c_str(), SumCount(fr, C_UNLOADS), SumCount(fr, C_SQUADS),
	         SumCount(fr, C_FXNEW), SumCount(fr, C_FXDEL),
	         V(zoneSM.p50, 2).c_str(), V(zoneSM.p90, 2).c_str(), V(unload.mx, 2).c_str(),
	         usFull.c_str(), usDraw.c_str());
	AuditOut(w);
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
// ---- [AUDIT-CURSOR] / [AUDIT-CURSPLIT]: the cursor ray per summary window ----

// Per-call cost of a subset of cursor calls. Frames carry per-call means; a
// frame with several calls counts once per call.
struct CurAcc
{
	int    n;          // calls
	double ms;         // per-call ms, summed over calls
	double sort;       // qsort ms, summed over the calls that have it
	int    sortN;
	float  mx;         // worst per-call ms
};

void CurAdd(CurAcc& a, int calls, float ms, float sort)
{
	a.n  += calls;
	a.ms += (double)ms * calls;
	if (!IsNan(sort))
	{
		a.sort  += (double)sort * calls;
		a.sortN += calls;
	}
	if (ms > a.mx)
		a.mx = ms;
}

std::string CurText(const char* name, const CurAcc& a)
{
	if (a.n <= 0)
		return Fmt(" %s.n=0", name);
	std::string s = Fmt(" %s.n=%d %s.ms=%.3f %s.max=%.2f", name, a.n, name, a.ms / a.n, name, (double)a.mx);
	if (a.sortN > 0)
		s += Fmt(" %s.sort=%.3f", name, a.sort / a.sortN);
	return s;
}

const int   CUR_HITBINS = 6;
const int   HITBIN_LO[CUR_HITBINS]   = { 0, 1, 10, 25, 50, 100 };
const char* HITBIN_NAME[CUR_HITBINS] = { "h0", "h1-9", "h10-24", "h25-49", "h50-99", "h100+" };

int HitBin(int hits)
{
	for (int i = CUR_HITBINS - 1; i > 0; --i)
		if (hits >= HITBIN_LO[i])
			return i;
	return 0;
}

struct GroupCount
{
	int    group;
	double hits;
};

bool GroupHitsGreater(const GroupCount& a, const GroupCount& b) { return a.hits > b.hits; }

// Over the window's running frames outside the exclusions (split frames are
// excluded, and summarised by WriteCursorSplit instead).
void WriteCursorSummary(const std::vector<const FrameRec*>& fr, double t0, double span)
{
	CurAcc all, cls[NUM_CURCLASSES], moved, still, btn, edge, mod, phys, idle, bins[CUR_HITBINS];
	memset(&all, 0, sizeof(all));
	memset(cls, 0, sizeof(cls));
	memset(&moved, 0, sizeof(moved));
	memset(&still, 0, sizeof(still));
	memset(&btn, 0, sizeof(btn));
	memset(&edge, 0, sizeof(edge));
	memset(&mod, 0, sizeof(mod));
	memset(&phys, 0, sizeof(phys));
	memset(&idle, 0, sizeof(idle));
	memset(bins, 0, sizeof(bins));
	std::vector<float> perFrame;
	double hits = 0.0, chars = 0.0, nChars = 0.0, ray2Ms = 0.0;
	double groups[CUR_GROUPS];
	for (int g = 0; g < CUR_GROUPS; ++g)
		groups[g] = 0.0;
	int hitsMax = 0, ray2 = 0, ray2Frames = 0;

	for (size_t i = 0; i < fr.size(); ++i)
	{
		const FrameRec& r = *fr[i];
		if (r.c[C_RAY2CALLS] > 0)
		{
			ray2 += r.c[C_RAY2CALLS];
			if (!IsNan(r.m[M_SUB_MOUSERAY2]))
			{
				ray2Ms += r.m[M_SUB_MOUSERAY2];
				++ray2Frames;
			}
		}
		int   calls = r.c[C_CURCALLS];
		float ms    = r.m[M_SUB_CURRAY];
		if (calls <= 0 || IsNan(ms))
			continue;
		float sort = r.m[M_SUB_CURSORT];
		perFrame.push_back(ms);
		CurAdd(all, calls, ms, sort);
		int k = r.c[C_CURLARGE] ? CUR_LARGE : (r.c[C_CURSMALL] ? CUR_SMALL : CUR_STILL);
		CurAdd(cls[k], calls, ms, sort);
		CurAdd(r.c[C_CURMOVED] ? moved : still, calls, ms, sort);
		if (r.c[C_CURBTN])  CurAdd(btn, calls, ms, sort);
		if (r.c[C_CUREDGE]) CurAdd(edge, calls, ms, sort);
		if (r.c[C_CURMOD])  CurAdd(mod, calls, ms, sort);
		CurAdd(r.c[C_CURPHYS] ? phys : idle, calls, ms, sort);
		int h = r.c[C_CURHITS] / calls;
		if (h > hitsMax) hitsMax = h;
		CurAdd(bins[HitBin(h)], calls, ms, sort);
		hits   += r.c[C_CURHITS];
		chars  += r.c[C_CURCHAR];
		nChars += (double)r.c[C_CHARS] * calls;
		for (int g = 0; g < CUR_GROUPS; ++g)
			groups[g] += r.curGroups[g];
	}
	if (all.n == 0 && ray2 == 0)
		return;

	std::string s = Fmt("[AUDIT-CURSOR] t=%.1f win=%.1fs frames=%d rayFrames=%d calls=%d charMask=0x%08X",
	                    t0, span, (int)fr.size(), (int)perFrame.size(), all.n, g_cfg.cursorCharGroups);
	if (all.n > 0)
	{
		double mean = all.ms / all.n;
		s += Fmt(" ms=%.3f p50=%.3f p90=%.3f max=%.2f", mean, (double)Percentile(perFrame, 0.50),
		         (double)Percentile(perFrame, 0.90), (double)all.mx);
		if (all.sortN > 0)
		{
			double sortMean = all.sort / all.sortN;
			s += Fmt(" sort=%.3f cast=%.3f sortShare=%.1f%%", sortMean, mean - sortMean,
			         mean > 0.0 ? 100.0 * sortMean / mean : 0.0);
		}
		double charsPerCall = nChars / all.n;
		s += Fmt(" hits=%.1f char=%.1f hitsMax=%d nChars=%.0f usPerChar=%s",
		         hits / all.n, chars / all.n, hitsMax, charsPerCall,
		         charsPerCall >= 1.0 ? Fmt("%.2f", mean * 1000.0 / charsPerCall).c_str() : "-");
	}
	s += Fmt(" ray2=%d ray2ms=%s", ray2, ray2Frames > 0 ? Fmt("%.3f", ray2Ms / ray2Frames).c_str() : "-");
	for (int k = 0; k < NUM_CURCLASSES; ++k)
		s += CurText(CURCLASS_NAMES[k], cls[k]);
	s += CurText("moved", moved) + CurText("unmoved", still);
	s += CurText("btn", btn) + CurText("edge", edge) + CurText("mod", mod);
	s += CurText("phys", phys) + CurText("physIdle", idle);
	for (int b = 0; b < CUR_HITBINS; ++b)
		s += CurText(HITBIN_NAME[b], bins[b]);

	std::vector<GroupCount> gc;
	for (int g = 0; g < CUR_GROUPS; ++g)
	{
		if (groups[g] <= 0.0)
			continue;
		GroupCount x;
		x.group = g;
		x.hits  = groups[g];
		gc.push_back(x);
	}
	std::sort(gc.begin(), gc.end(), GroupHitsGreater);
	std::string gl;
	for (size_t i = 0; i < gc.size() && i < 10; ++i)
	{
		if (!gl.empty()) gl += ",";
		gl += gc[i].group < 32 ? Fmt("%d:%.0f", gc[i].group, gc[i].hits) : Fmt("none:%.0f", gc[i].hits);
	}
	s += " groups=" + (gl.empty() ? std::string("-") : gl);
	AuditOut(s);
}

// Split frames of the window: the game's call against the audit's five shadow
// rays (same origin, direction and scene API).
void WriteCursorSplit(double t0, double span)
{
	if (g_cfg.cursorSplitEvery <= 0)
		return;
	std::vector<float> ms[NUM_SPLITRAYS];
	double hits[NUM_SPLITRAYS], chars[NUM_SPLITRAYS];
	for (int i = 0; i < NUM_SPLITRAYS; ++i)
		hits[i] = chars[i] = 0.0;
	int n = 0, physStart = 0, physEnd = 0, sortN = 0;
	double sortSum = 0.0, castSum = 0.0, castMinusFull = 0.0, dynAB = 0.0;
	for (size_t f = 0; f < g_window.size(); ++f)
	{
		const FrameRec& r = g_window[f];
		if (!(r.flags & F_CURSORSPLIT))
			continue;
		const CursorSplit& sp = r.split;
		++n;
		for (int i = 0; i < NUM_SPLITRAYS; ++i)
		{
			ms[i].push_back(sp.ms[i]);
			hits[i]  += sp.hits[i];
			chars[i] += sp.chars[i];
		}
		if (!IsNan(sp.realSortMs))
		{
			double cast = sp.ms[SR_REAL] - sp.realSortMs;
			sortSum       += sp.realSortMs;
			castSum       += cast;
			castMinusFull += cast - sp.ms[SR_FULL];
			++sortN;
		}
		dynAB     += sp.ms[SR_DYNA] - sp.ms[SR_DYNB];
		physStart += sp.physAtStart;
		physEnd   += sp.physAtEnd;
	}
	std::string s = Fmt("[AUDIT-CURSPLIT] t=%.1f win=%.1fs n=%d every=%d charMask=0x%08X", t0, span, n,
	                    g_cfg.cursorSplitEvery, g_cfg.cursorCharGroups);
	if (n > 0)
	{
		s += Fmt(" physStart=%d physEnd=%d", physStart, physEnd);
		if (sortN > 0)
			s += Fmt(" realSort=%.3f realCast=%.3f realCast-full=%.3f", sortSum / sortN,
			         castSum / sortN, castMinusFull / sortN);
		s += Fmt(" dynA-dynB=%.3f", dynAB / n);
		for (int i = 0; i < NUM_SPLITRAYS; ++i)
		{
			double sum = 0.0;
			float  mx  = 0.0f;
			for (size_t k = 0; k < ms[i].size(); ++k)
			{
				sum += ms[i][k];
				if (ms[i][k] > mx) mx = ms[i][k];
			}
			const char* name = SPLITRAY_NAMES[i];
			s += Fmt(" %s.ms=%.3f %s.p50=%.3f %s.max=%.2f %s.hits=%.1f %s.char=%.1f",
			         name, sum / n, name, (double)Percentile(ms[i], 0.50), name, (double)mx,
			         name, hits[i] / n, name, chars[i] / n);
		}
	}
	AuditOut(s);
}

void WriteSummary()
{
	if (g_window.empty())
		return;

	std::vector<const FrameRec*> byClass[NUM_CLASSES];
	int excluded = 0;
	float sumErrMax = 0.0f;
	for (size_t i = 0; i < g_window.size(); ++i)
	{
		const FrameRec& r = g_window[i];
		float e = r.m[M_SUMERR];
		if (!IsNan(e) && fabs(e) > sumErrMax) sumErrMax = (float)fabs(e);
		if (Excluded(r)) { ++excluded; continue; }
		byClass[ClassOf(r)].push_back(&r);
	}

	double span = g_window.back().t - g_window.front().t;
	if (g_window.size() > 1 && !IsNan(g_window.back().m[M_FRAME]))
		span += g_window.back().m[M_FRAME] / 1000.0;
	double fps = span > 0.0 ? g_window.size() / span : 0.0;

	AuditOut(Fmt("[AUDIT] t=%.1f win=%.1fs frames=%d fps=%.1f steady=%d stream=%d paused=%d menu=%d excl=%d lost=%ld sumErrMax=%.3f slow=%d(+%d suppressed)",
	             g_window.front().t, span, (int)g_window.size(), fps,
	             (int)byClass[CLS_STEADY].size(), (int)byClass[CLS_STREAM].size(),
	             (int)byClass[CLS_PAUSED].size(), (int)byClass[CLS_MENU].size(),
	             excluded, (long)g_ringLost, sumErrMax, g_slowThisWindow, g_slowSuppressed));

	if (!byClass[CLS_STEADY].empty())
		WriteClassSummary("steady", byClass[CLS_STEADY]);
	if (byClass[CLS_STREAM].size() >= 30)
		WriteClassSummary("stream", byClass[CLS_STREAM]);
	WriteRenderBuckets();
	WriteDrawTables();
	WriteTaskTable();
	WriteFxTable();
	{
		std::vector<const FrameRec*> running(byClass[CLS_STEADY]);
		running.insert(running.end(), byClass[CLS_STREAM].begin(), byClass[CLS_STREAM].end());
		WriteCursorSummary(running, g_window.front().t, span);
		WriteCursorSplit(g_window.front().t, span);
	}

	g_timer = MeasureTimer();
	WriteTimerLine();

	g_window.clear();
	g_windowStart    = -1.0;
	g_slowThisWindow = 0;
	g_slowSuppressed = 0;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
