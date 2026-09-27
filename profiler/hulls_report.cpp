// hulls_report.cpp - Hull anomaly, snapshot and counter printers.
// Runs on the main thread (Hulls_OnFrameStarted); takes no hull diagnostic lock.

#include "hulls_detail.h"

namespace audit {
namespace audithulls_detail {

// ---- Main-thread reporting ------------------------------------------------

std::string Where(uintptr_t a)
{
	if (!a)
		return "?";
	if (a >= g_exeBase && a < g_exeEnd)
		return Fmt("exe+0x%IX", a - g_exeBase);
	uintptr_t mb = 0, me = 0;
	if (ModuleRange((const void*)a, &mb, &me))
	{
		char path[MAX_PATH];
		DWORD n = GetModuleFileNameA((HMODULE)mb, path, MAX_PATH);
		const char* file = "?";
		if (n > 0 && n < MAX_PATH)
		{
			const char* slash = strrchr(path, '\\');
			file = slash ? slash + 1 : path;
		}
		return Fmt("%s+0x%IX", file, a - mb);
	}
	return Fmt("0x%IX", a);
}

std::string ThreadName(DWORD tid)
{
	const char* role = tid == g_mainThreadId ? "main" : (tid == g_physTid ? "phys" : "other");
	return Fmt("%lu(%s)", (unsigned long)tid, role);
}

std::string FlagNames(unsigned f)
{
	std::string s;
	if (f & F_OFFMAIN)  s += ",offMain";
	if (f & F_MOVABLE)  s += ",movable";
	if (f & F_DELETE)   s += ",delete";
	if (f & F_CONSUMER) s += ",consumer";
	return s.empty() ? std::string("-") : s.substr(1);
}

std::string Ago(LONG64 now, LONG64 then)
{
	if (!then)
		return "-";
	return Fmt("%.1fms", TicksToMs(now - then));
}

std::string NearestUnload(LONG64 at)
{
	int n = g_unloadCount < UNLOADS ? g_unloadCount : UNLOADS;
	int best = -1;
	LONG64 bestD = 0;
	for (int i = 0; i < n; ++i)
	{
		LONG64 d = g_unloads[i].qpc - at;
		if (d < 0)
			d = -d;
		if (best < 0 || d < bestD)
		{
			best = i;
			bestD = d;
		}
	}
	if (best < 0)
		return "none";
	const UnloadStamp& u = g_unloads[best];
	return Fmt("(%d,%d)@%+.1fms", u.x, u.y, TicksToMs(u.qpc - at));
}

std::string Describe(const HullRec& r)
{
	const char* name = r.kind < EV_COUNT ? EVENT_NAMES[r.kind] : "?";
	if (r.kind == EV_FLUSH || r.kind == EV_BATCH)
		return Fmt("t=%.3f %s n=%Iu tid=%s", SinceStart(r.qpc), name, r.ptr, ThreadName(r.tid).c_str());
	return Fmt("t=%.3f %s ptr=0x%IX tid=%s caller=%s vt=%s flags=%s", SinceStart(r.qpc), name, r.ptr,
	           ThreadName(r.tid).c_str(), Where(r.ret).c_str(),
	           r.vtRva ? Fmt("exe+0x%X", r.vtRva).c_str() : "?", FlagNames(r.flags).c_str());
}

void PrintAnomalies()
{
	for (int k = 0; k < AN_COUNT; ++k)
	{
		while (g_anomPrinted[k] < ANOM_KEEP && g_anom[k][g_anomPrinted[k]].ready)
		{
			const AnomRec& a = g_anom[k][g_anomPrinted[k]];
			++g_anomPrinted[k];
			int st = StateOf(a.prev);
			unsigned caller = CallerOf(a.prev);
			std::string prevCaller = caller == 0 ? std::string("?")
				: (caller == 0xFFFFFFFFu ? std::string("outside-exe") : Fmt("exe+0x%X", caller));
			int prevKind = KindOf(a.prev);
			AuditLine(Fmt("[AUDIT-HULLS] anomaly=%s #%d ", ANOMALY_NAMES[k], g_anomPrinted[k]) +
			          Describe(a.ev) +
			          Fmt(" prev=%s/%s extra=%d prevCaller=%s pushAgo=%s dtorAgo=%s unload=%s",
			              st < ST_COUNT ? STATE_NAMES[st] : "?",
			              prevKind < EV_COUNT && st != ST_NONE && st != ST_MADE ? EVENT_NAMES[prevKind] : "-",
			              ExtraOf(a.prev), prevCaller.c_str(),
			              Ago(a.ev.qpc, a.prevPushQpc).c_str(), Ago(a.ev.qpc, a.prevDtorQpc).c_str(),
			              NearestUnload(a.ev.qpc).c_str()));
		}
	}
}

void PrintSnapshot(int an)
{
	AuditLine(Fmt("[AUDIT-HULLS] ring %s: the %d events up to its first anomaly (event #%I64d)",
	              ANOMALY_NAMES[an], SNAP, g_snapEnd[an]));
	for (int k = 0; k < SNAP; ++k)
	{
		LONG64 i = g_snapEnd[an] - SNAP + k;
		if (i < 0)
			continue;
		const HullRec& r = g_snap[an][k];
		if (r.seq != i + 1)
		{
			AuditLine(Fmt("[AUDIT-HULLS]   #%I64d overwritten or torn", i + 1));
			continue;
		}
		AuditLine(Fmt("[AUDIT-HULLS]   #%I64d ", i + 1) + Describe(r) + " unload=" + NearestUnload(r.qpc));
	}
}

void PrintStats()
{
	AuditLine(Fmt("[AUDIT-HULLS] push=%ld/%ld dtor=%ld/%ld dupPush=%ld pushAfterDtor=%ld offMainPush=%ld "
	              "dtorUnqueued=%ld overflow=%ld pushOther=%ld/%ld/%ld/%ld/%ld dtorOther=%ld/%ld/%ld/%ld/%ld/%ld "
	              "dtorConsumer=%ld dtorQueued=%ld batchUnqueued=%ld makeQueued=%ld made=%ld flushes=%ld "
	              "batches=%ld batchMax=%ld batchEntries=%I64d events=%I64d",
	              g_push[EV_PUSH_HULL], g_push[EV_PUSH_ENTITY],
	              g_dtor[EV_DTOR_HULL - EV_DTOR_HULL], g_dtor[EV_DTOR_SIMPLE - EV_DTOR_HULL],
	              g_anomCount[AN_DUP_PUSH], g_anomCount[AN_PUSH_AFTER_DTOR], g_anomCount[AN_OFF_MAIN_PUSH],
	              g_anomCount[AN_DTOR_UNQUEUED], g_table.overflow,
	              g_push[EV_PUSH_SCYTHE], g_push[EV_PUSH_ROOT], g_push[EV_PUSH_BASE],
	              g_push[EV_PUSH_UNLOAD], g_push[EV_PUSH_INLINE],
	              g_dtor[EV_DTOR_BOX - EV_DTOR_HULL], g_dtor[EV_DTOR_CAPSULE - EV_DTOR_HULL],
	              g_dtor[EV_DTOR_DOOR - EV_DTOR_HULL], g_dtor[EV_DTOR_SCYTHE - EV_DTOR_HULL],
	              g_dtor[EV_DTOR_ROOT - EV_DTOR_HULL], g_dtor[EV_DTOR_RAGDOLL - EV_DTOR_HULL],
	              g_dtorConsumer, g_anomCount[AN_DTOR_QUEUED], g_anomCount[AN_BATCH_UNQUEUED],
	              g_anomCount[AN_MAKE_QUEUED], g_made, g_flushes, g_batches, g_batchMax,
	              g_batchEntries, g_ringHead));
}

} // namespace
using namespace audithulls_detail;

} // namespace audit
