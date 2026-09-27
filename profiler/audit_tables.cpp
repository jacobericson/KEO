// audit_tables.cpp - Render, task and particle summary tables.
// Reporter thread; snapshots published counters without taking locks.

#include "audit_detail.h"
#include <algorithm>

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
int                 g_bucketFrames = 0;

void AccumulateBuckets(const FrameRec& r)
{
	++g_bucketFrames;
	for (int i = 0; i < r.ncalls && i < MAX_RCALLS; ++i)
	{
		const RCall& c = r.calls[i];
		unsigned char key = (unsigned char)(c.flags & (RC_SHADOW | RC_CULLONLY));
		Bucket* b = NULL;
		for (size_t k = 0; k < g_buckets.size(); ++k)
		{
			Bucket& x = g_buckets[k];
			if (x.cam == c.cam && x.vp == c.vp && x.firstRq == c.firstRq && x.lastRq == c.lastRq &&
			    (x.flags & (RC_SHADOW | RC_CULLONLY)) == key)
			{
				b = &x;
				break;
			}
		}
		if (!b)
		{
			if (g_buckets.size() >= 64)
				continue;
			Bucket nb;
			memset(&nb, 0, sizeof(nb));
			nb.cam = c.cam; nb.vp = c.vp; nb.firstRq = c.firstRq; nb.lastRq = c.lastRq;
			g_buckets.push_back(nb);
			b = &g_buckets.back();
		}
		b->flags |= c.flags;
		++b->calls;
		b->ms       += c.ms;
		b->cullMs   += c.cullMs;
		b->submitMs += c.submitMs;
		if (!IsNan(c.rsoMs))
		{
			b->rsoMs     += c.rsoMs;
			b->setPassMs += c.setPassMs;
			b->timedRsos += c.rsos;
			++b->timedCalls;
		}
		if (!IsNan(c.bindMs)) { b->bindMs += c.bindMs; ++b->bindN; }
		if (!IsNan(c.d3dMs))  { b->d3dMs  += c.d3dMs;  ++b->d3dN; }
		if (!IsNan(c.syncMs)) { b->syncMs += c.syncMs; ++b->syncN; }
		b->draws     += c.draws;
		b->instDraws += c.instDraws;
		b->instances += c.instances;
		b->setPasses += c.setPasses;
		b->rsos      += c.rsos;
		b->noRso     += c.noRso;
		b->attachRso += c.attachRso;
	}
}

// Per-frame mean of a per-call value that is missing on some calls: scale
// the calls that had it up to all calls.
std::string ScaledPerFrame(double sum, int have, int calls, double frames)
{
	if (have <= 0 || frames <= 0.0)
		return "-";
	return Fmt("%.2f", sum * ((double)calls / have) / frames);
}

bool BucketCostGreater(const Bucket& a, const Bucket& b)
{
	return a.ms + a.cullMs > b.ms + b.cullMs;
}

void WriteRenderBuckets()
{
	if (g_bucketFrames > 0 && !g_buckets.empty())
	{
		std::sort(g_buckets.begin(), g_buckets.end(), BucketCostGreater);
		double f = (double)g_bucketFrames;
		LONG camN = g_camN, vpN = g_vpN;
		AuditOut(Fmt("[AUDIT-RENDER] frames=%d buckets=%d (per-frame means over running frames; ms exclusive of nested scene calls)",
		             g_bucketFrames, (int)g_buckets.size()));
		for (size_t i = 0; i < g_buckets.size() && i < 16; ++i)
		{
			const Bucket& b = g_buckets[i];
			double cpf = b.calls / f;
			std::string rso = "-", setPass = "-", usRso = "-";
			if (b.timedCalls > 0)
			{
				double share = (double)b.calls / b.timedCalls;   // scale the timed calls to all calls
				rso     = Fmt("%.2f", b.rsoMs * share / f);
				setPass = Fmt("%.2f", b.setPassMs * share / f);
				if (b.timedRsos > 0.5)
					usRso = Fmt("%.2f", b.rsoMs * 1000.0 / b.timedRsos);
			}
			std::string line = Fmt("[AUDIT-RENDER] %s%s cam=%s vp=%s rq=%d-%d calls/f=%.2f ms=%.2f cull=%.2f queue=%.2f submit=%.2f rso=%s setPass=%s",
			                       (b.flags & RC_SHADOW) ? "shadow" : "main",
			                       (b.flags & RC_CULLONLY) ? "(cull only)" : ((b.flags & RC_NESTED) ? "(nested)" : ""),
			                       b.cam < camN ? g_camE[b.cam].name : "?", b.vp < vpN ? g_vpE[b.vp].name : "?",
			                       (int)b.firstRq, (int)b.lastRq, cpf,
			                       b.ms / f, b.cullMs / f, (b.ms - b.submitMs) / f, b.submitMs / f,
			                       rso.c_str(), setPass.c_str());
			line += Fmt(" bind=%s d3d=%s sync=%s",
			            ScaledPerFrame(b.bindMs, b.bindN, b.calls, f).c_str(),
			            ScaledPerFrame(b.d3dMs, b.d3dN, b.calls, f).c_str(),
			            ScaledPerFrame(b.syncMs, b.syncN, b.calls, f).c_str());
			line += Fmt(" draws=%.0f instDraws=%.0f inst=%.0f setPass#=%.0f rso#=%.0f noRso=%.0f attach#=%.0f usPerRso=%s",
			            b.draws / f, b.instDraws / f, b.instances / f, b.setPasses / f, b.rsos / f,
			            b.noRso / f, b.attachRso / f, usRso.c_str());
			AuditOut(line);
		}
	}
	g_buckets.clear();
	g_bucketFrames = 0;
}

// Draws by Renderable class and by mesh: deltas of the main thread's counters
// since the previous summary, per rendered frame.
LONG   g_prevDrawFrames = 0;
LONG   g_prevClsDraws[2][MAX_CLASSES];
LONG   g_prevClsInst[2][MAX_CLASSES];
LONG64 g_prevClsTsc[2][MAX_CLASSES];
LONG   g_prevMeshDraws[2][MAX_MESHES];
LONG   g_prevMeshBatch[2][MAX_MESHES];
LONG   g_prevMeshInst[2][MAX_MESHES];

struct DrawRow
{
	int    id;
	double d[2], inst[2], batch[2], us[2];
};

bool RowTotalGreater(const DrawRow& a, const DrawRow& b)  { return a.d[0] + a.d[1] > b.d[0] + b.d[1]; }
bool RowMainGreater(const DrawRow& a, const DrawRow& b)   { return a.d[0] > b.d[0]; }
bool RowShadowGreater(const DrawRow& a, const DrawRow& b) { return a.d[1] > b.d[1]; }

std::string UsOrDash(double us)
{
	return us < 0.0 ? std::string("-") : Fmt("%.2f", us);
}

void WriteDrawTables()
{
	if (!g_cfg.renderDetail || g_cfg.drawTop <= 0)
		return;
	LONG frames = g_drawFrames;
	LONG df = frames - g_prevDrawFrames;
	g_prevDrawFrames = frames;
	if (df <= 0)
		return;
	double f = (double)df;
	double perMs = g_tscPerMs;

	std::vector<DrawRow> rows;
	LONG ncls = g_clsN;
	for (LONG id = 0; id < ncls && id < MAX_CLASSES; ++id)
	{
		DrawRow r;
		memset(&r, 0, sizeof(r));
		r.id = (int)id;
		bool any = false;
		for (int s = 0; s < 2; ++s)
		{
			LONG   d = g_clsDraws[s][id], in = g_clsInst[s][id];
			LONG64 t = g_clsTsc[s][id];
			LONG   dd = d - g_prevClsDraws[s][id];
			LONG   di = in - g_prevClsInst[s][id];
			LONG64 dt = t - g_prevClsTsc[s][id];
			g_prevClsDraws[s][id] = d; g_prevClsInst[s][id] = in; g_prevClsTsc[s][id] = t;
			r.d[s]    = dd / f;
			r.inst[s] = di / f;
			r.us[s]   = (dd > 0 && perMs > 0.0) ? (double)dt / perMs * 1000.0 / dd : -1.0;
			if (dd > 0) any = true;
		}
		if (any)
			rows.push_back(r);
	}
	std::sort(rows.begin(), rows.end(), RowTotalGreater);
	AuditOut(Fmt("[AUDIT-DRAWS] frames=%d classes=%ld meshes=%ld cameras=%ld viewports=%ld | per frame: draws inst us/draw (main | shadow)",
	             (int)df, (long)g_clsN, (long)g_meshN, (long)g_camN, (long)g_vpN));
	for (size_t i = 0; i < rows.size() && (int)i < g_cfg.drawTop; ++i)
	{
		const DrawRow& r = rows[i];
		AuditOut(Fmt("[AUDIT-DRAWS] %s main %.1f %.1f %s | shadow %.1f %.1f %s",
		             g_clsE[r.id].name, r.d[0], r.inst[0], UsOrDash(r.us[0]).c_str(),
		             r.d[1], r.inst[1], UsOrDash(r.us[1]).c_str()));
	}

	rows.clear();
	LONG nmesh = g_meshN;
	for (LONG id = 0; id < nmesh && id < MAX_MESHES; ++id)
	{
		DrawRow r;
		memset(&r, 0, sizeof(r));
		r.id = (int)id;
		bool any = false;
		for (int s = 0; s < 2; ++s)
		{
			LONG d = g_meshDraws[s][id], b = g_meshBatch[s][id], in = g_meshInst[s][id];
			LONG dd = d - g_prevMeshDraws[s][id];
			LONG db = b - g_prevMeshBatch[s][id];
			LONG di = in - g_prevMeshInst[s][id];
			g_prevMeshDraws[s][id] = d; g_prevMeshBatch[s][id] = b; g_prevMeshInst[s][id] = in;
			r.d[s]     = dd / f;
			r.batch[s] = db / f;
			r.inst[s]  = di / f;
			if (dd > 0) any = true;
		}
		if (any)
			rows.push_back(r);
	}
	for (int s = 0; s < 2; ++s)
	{
		std::sort(rows.begin(), rows.end(), s == 0 ? RowMainGreater : RowShadowGreater);
		for (size_t i = 0; i < rows.size() && (int)i < g_cfg.drawTop; ++i)
		{
			const DrawRow& r = rows[i];
			if (r.d[s] <= 0.0)
				break;
			AuditOut(Fmt("[AUDIT-MESH] %s #%d %s draws/f=%.1f batchDraws/f=%.1f inst/f=%.1f | %s draws/f=%.1f",
			             s == 0 ? "main" : "shadow", (int)i + 1, g_meshE[r.id].name,
			             r.d[s], r.batch[s], r.inst[s], s == 0 ? "shadow" : "main", r.d[1 - s]));
		}
	}
}

// List-1 task execution by task class, per collected AI run.
LONG   g_prevAiFrames = 0;
LONG64 g_prevTaskTicks[MAX_CLASSES];
LONG   g_prevTaskCalls[MAX_CLASSES];
LONG64 g_prevTaskNoneTicks = 0;
LONG   g_prevTaskNoneCalls = 0;

struct TaskRow
{
	int    id;
	double ms, calls;
};

bool TaskMsGreater(const TaskRow& a, const TaskRow& b) { return a.ms > b.ms; }

void WriteTaskTable()
{
	if (g_cfg.drawTop <= 0)
		return;
	LONG frames = g_aiFrames;
	LONG df = frames - g_prevAiFrames;
	g_prevAiFrames = frames;
	if (df <= 0)
		return;
	double f = (double)df;

	std::vector<TaskRow> rows;
	LONG ncls = g_clsN;
	for (LONG id = 0; id < ncls && id < MAX_CLASSES; ++id)
	{
		LONG64 t = g_taskTicks[id];
		LONG   c = g_taskCalls[id];
		LONG64 dt = t - g_prevTaskTicks[id];
		LONG   dc = c - g_prevTaskCalls[id];
		g_prevTaskTicks[id] = t;
		g_prevTaskCalls[id] = c;
		if (dc > 0)
		{
			TaskRow r;
			r.id    = (int)id;
			r.ms    = TicksToMs(dt) / f;
			r.calls = dc / f;
			rows.push_back(r);
		}
	}
	LONG64 nt = g_taskNoneTicks;
	LONG   nc = g_taskNoneCalls;
	double noneMs    = TicksToMs(nt - g_prevTaskNoneTicks) / f;
	double noneCalls = (nc - g_prevTaskNoneCalls) / f;
	g_prevTaskNoneTicks = nt;
	g_prevTaskNoneCalls = nc;
	if (rows.empty() && noneCalls <= 0.0)
		return;

	std::sort(rows.begin(), rows.end(), TaskMsGreater);
	AuditOut(Fmt("[AUDIT-AI] runs=%d task execution in list 1 (CharBody::update), per AI run: classes=%d no-task calls/run=%.1f ms/run=%.3f",
	             (int)df, (int)rows.size(), noneCalls, noneMs));
	for (size_t i = 0; i < rows.size() && (int)i < g_cfg.drawTop; ++i)
	{
		const TaskRow& r = rows[i];
		AuditOut(Fmt("[AUDIT-AI] %s calls/run=%.1f ms/run=%.3f us/call=%.1f",
		             g_clsE[r.id].name, r.calls, r.ms, r.calls > 0.0 ? r.ms * 1000.0 / r.calls : 0.0));
	}
}

// Particle effects by template: deltas of the census and _update counters
// since the previous summary. States (alive, on screen, stopping, particles)
// are per census frame; entries and exits are per second of unpaused time.
LONG   g_prevFxFrames    = 0;
double g_prevFxRunMs     = 0.0;
LONG64 g_prevFxEff       = 0;
LONG64 g_prevFxNvto      = 0;
LONG64 g_prevFxMainTsc   = 0;
LONG   g_prevFxMainCalls = 0;
LONG64 g_prevFxTsc[2][MAX_FX_TPL];
LONG   g_prevFxCalls[2][MAX_FX_TPL];
LONG64 g_prevFxAlive[MAX_FX_TPL];
LONG64 g_prevFxVis[MAX_FX_TPL];
LONG64 g_prevFxStop[MAX_FX_TPL];
LONG64 g_prevFxPart[MAX_FX_TPL];
LONG   g_prevFxNew[MAX_FX_TPL];
LONG   g_prevFxDel[MAX_FX_TPL];

struct FxRow
{
	int    id;
	double alive, vis, stop, parts;   // sums over the census frames
	double news, dels, calls;         // counts
	double ticks, offTicks;           // _update TSC ticks
};

bool FxTicksGreater(const FxRow& a, const FxRow& b) { return a.ticks > b.ticks; }

// Per-second rate over unpaused time, or "-" when there was too little of it.
std::string Rate(double count, double sec)
{
	return sec >= 0.5 ? Fmt("%.2f", count / sec) : std::string("-");
}

std::string TicksMs(double ticks, double perMs, double frames)
{
	return (perMs > 0.0 && frames > 0.0) ? Fmt("%.3f", ticks / perMs / frames) : std::string("-");
}

void WriteFxTable()
{
	if (!g_cfg.particles)
		return;
	LONG frames = g_fxFrames;
	double f = (double)(frames - g_prevFxFrames);
	g_prevFxFrames = frames;
	double runMs = g_fxRunMs;
	double sec = (runMs - g_prevFxRunMs) / 1000.0;
	g_prevFxRunMs = runMs;

	LONG64 eff = g_fxEffSum, nvto = g_fxNvtoSum, mainTsc = g_fxMainTsc;
	LONG mainCalls = g_fxMainCalls;
	double dEff = (double)(eff - g_prevFxEff), dNvto = (double)(nvto - g_prevFxNvto);
	double dMainTicks = (double)(mainTsc - g_prevFxMainTsc), dMainCalls = (double)(mainCalls - g_prevFxMainCalls);
	g_prevFxEff = eff; g_prevFxNvto = nvto; g_prevFxMainTsc = mainTsc; g_prevFxMainCalls = mainCalls;

	std::vector<FxRow> rows;
	FxRow tot;
	memset(&tot, 0, sizeof(tot));
	LONG n = g_fxN;
	for (LONG id = 0; id < n && id < MAX_FX_TPL; ++id)
	{
		FxRow r;
		memset(&r, 0, sizeof(r));
		r.id = (int)id;
		for (int s = 0; s < 2; ++s)
		{
			LONG64 t = g_fxTsc[s][id];
			LONG   c = g_fxCalls[s][id];
			double dt = (double)(t - g_prevFxTsc[s][id]);
			r.ticks += dt;
			if (s == 1) r.offTicks = dt;
			r.calls += (double)(c - g_prevFxCalls[s][id]);
			g_prevFxTsc[s][id] = t;
			g_prevFxCalls[s][id] = c;
		}
		LONG64 a = g_fxAlive[id], v = g_fxVisSum[id], st = g_fxStopSum[id], p = g_fxPartSum[id];
		LONG nw = g_fxNew[id], dl = g_fxDel[id];
		r.alive = (double)(a - g_prevFxAlive[id]);   g_prevFxAlive[id] = a;
		r.vis   = (double)(v - g_prevFxVis[id]);     g_prevFxVis[id]   = v;
		r.stop  = (double)(st - g_prevFxStop[id]);   g_prevFxStop[id]  = st;
		r.parts = (double)(p - g_prevFxPart[id]);    g_prevFxPart[id]  = p;
		r.news  = (double)(nw - g_prevFxNew[id]);    g_prevFxNew[id]   = nw;
		r.dels  = (double)(dl - g_prevFxDel[id]);    g_prevFxDel[id]   = dl;
		tot.alive += r.alive; tot.vis += r.vis; tot.stop += r.stop; tot.parts += r.parts;
		tot.news += r.news; tot.dels += r.dels; tot.calls += r.calls;
		tot.ticks += r.ticks; tot.offTicks += r.offTicks;
		if (r.alive > 0.0 || r.calls > 0.0 || r.news > 0.0 || r.dels > 0.0)
			rows.push_back(r);
	}
	if (f <= 0.0 && tot.calls <= 0.0)
		return;

	double perMs = g_tscPerMs;
	double pf = f > 0.0 ? f : 1.0;
	std::sort(rows.begin(), rows.end(), FxTicksGreater);
	AuditOut(Fmt("[AUDIT-FX] frames=%.0f run=%.1fs effects/f=%.1f withParticles/f=%.1f vis/f=%.1f stop/f=%.1f parts/f=%.0f nvTimeout/f=%.1f new/s=%s del/s=%s calls/f=%.1f upd=%s ms/f (offscreen %s, main thread %s in %.1f calls/f) templates=%ld%s",
	             f, sec, dEff / pf, tot.alive / pf, tot.vis / pf, tot.stop / pf, tot.parts / pf, dNvto / pf,
	             Rate(tot.news, sec).c_str(), Rate(tot.dels, sec).c_str(), tot.calls / pf,
	             TicksMs(tot.ticks, perMs, f).c_str(), TicksMs(tot.offTicks, perMs, f).c_str(),
	             TicksMs(dMainTicks, perMs, f).c_str(), dMainCalls / pf, (long)n - 1,
	             g_fxOverflow ? " MAP-FULL(entries/exits undercounted)" : ""));
	for (size_t i = 0; i < rows.size() && (int)i < g_cfg.particleTop; ++i)
	{
		const FxRow& r = rows[i];
		// Mean time in the active list (Little's law: alive / entry rate).
		std::string life = "-";
		if (sec >= 0.5 && r.news > 0.0 && f > 0.0)
			life = Fmt("%.1fs", (r.alive / f) / (r.news / sec));
		std::string usCall = (perMs > 0.0 && r.calls > 0.0) ? Fmt("%.1f", r.ticks / perMs * 1000.0 / r.calls) : std::string("-");
		AuditOut(Fmt("[AUDIT-FX] %s alive=%.1f vis=%.1f stop=%.1f parts=%.0f new/s=%s del/s=%s life=%s calls/f=%.1f ms/f=%s off=%s us/call=%s",
		             g_fxE[r.id].name, r.alive / pf, r.vis / pf, r.stop / pf, r.parts / pf,
		             Rate(r.news, sec).c_str(), Rate(r.dels, sec).c_str(), life.c_str(), r.calls / pf,
		             TicksMs(r.ticks, perMs, f).c_str(), TicksMs(r.offTicks, perMs, f).c_str(), usCall.c_str()));
	}
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
