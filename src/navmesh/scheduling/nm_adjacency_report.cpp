// nm_adjacency_report.cpp - main-thread adjacency heartbeat.
// The 60-second report uses FixedLogBuf and prints three lines.
#include "navmesh/scheduling/nm_adjacency_internal.h"
#include "base/fixed_log_buf.h"
namespace nm_adjacency_detail {
static const double kBeatSeconds = 60.0;
} // namespace nm_adjacency_detail
using namespace nm_adjacency_detail;
static void Field(FixedLogBuf* o, const char* name, LONG v, bool live)
{
	FlbStr(o, name);
	if (live) FlbDec(o, v); else FlbChar(o, '?');
}

static void FieldMs(FixedLogBuf* o, const char* name, LONG64 us, bool live)
{
	FlbStr(o, name);
	if (!live) { FlbChar(o, '?'); return; }
	FlbDec(o, us / 1000); FlbChar(o, '.'); FlbDec(o, (us / 100) % 10);
}

static const char* P90Bucket()
{
	static const char* names[8] = { "<1", "<2", "<5", "<10", "<20", "<50", "<200", ">=200" };
	LONG n = 0, h[8];
	for (int i = 0; i < 8; ++i) { h[i] = Read(&s_bgWaitHist[i]); n += h[i]; }
	if (!n) return "-";
	LONG need = n - n / 10, acc = 0;
	for (int i = 0; i < 8; ++i) { acc += h[i]; if (acc >= need) return names[i]; }
	return names[7];
}

void NavMeshAdjTick(double now)
{
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;

	const bool live = s_mode != MODE_OFF;
	const bool obs = s_observerInstalled;
	FixedLogBuf o; FlbInit(&o);
	FlbStr(&o, "NavMeshAdj: mode=");
	FlbStr(&o, s_mode == MODE_ENFORCE ? "enforce" : (s_mode == MODE_OFF ? "off" : "count"));
	if (!obs) { FlbStr(&o, "(observer "); FlbStr(&o, s_observerWhy ? s_observerWhy : "?"); FlbChar(&o, ')'); }
	Field(&o, " calls=",      Read(&s_calls), obs);
	Field(&o, " claims=",     Read(&s_claims), live);
	Field(&o, " adjSkip=",    Read(&s_skips), live);
	Field(&o, " skipClaims=", Read(&s_skipClaims), live);
	Field(&o, " adjWould=",   Read(&s_would), live);
	Field(&o, " adjDeferred=", Read(&s_deferred), live);
	FieldMs(&o, " deferMaxMs=", Read64(&s_deferMaxUs), live);
	Field(&o, " adjBgWait=",  Read(&s_bgWaits), live);
	FieldMs(&o, " totMs=",    Read64(&s_bgWaitUs), live);
	FieldMs(&o, " maxMs=",    Read64(&s_bgWaitMaxUs), live);
	FlbStr(&o, " p90Ms=");    FlbStr(&o, live ? P90Bucket() : "?");
	LogMsgDeferrable(FlbDone(&o));

	// The registry and the drain on a line of their own, so neither overruns
	// the buffer.
	FlbInit(&o);
	FlbStr(&o, "NavMeshAdj registry:");
	Field(&o, " adjFull=",    Read(&s_full), live);
	Field(&o, " resAge=",     Read(&s_resAge), live);
	Field(&o, " resDrop=",    Read(&s_resDrop), live);
	Field(&o, " bgSkip=",     Read(&s_bgSkip), live);
	Field(&o, " bgIdle=",     Read(&s_bgIdle), live);
	Field(&o, " bgPassFull=", Read(&s_bgPassFull), live);
	Field(&o, " bgoClaims=",  Read(&s_bgoClaims), live);
	FieldMs(&o, " bgoAgeMaxMs=", Read64(&s_bgoMaxUs), live);
	Field(&o, " liveMax=",    Read(&s_liveMax), live);
	Field(&o, " pubMax=",     Read(&s_pubMax), live);
	Field(&o, " pubNow=",     Read(&g_pubHint), live);
	Field(&o, " drained=",    Read(&s_obsFreed), live && obs);
	Field(&o, " obsOverflow=", Read(&s_obsOverflow), live && obs);
	Field(&o, " otherNmg=",   Read(&s_obsOtherNmg), live && obs);
	LogMsgDeferrable(FlbDone(&o));

	const bool chk = live && Read(&s_checkerPresent) != 0;
	FixedLogBuf p; FlbInit(&p);
	FlbStr(&p, "NavMeshAdj check:");
	if (!Read(&s_checkerPresent)) FlbStr(&p, " (stitch detour not installed)");
	Field(&p, " adjViol=",   Read(&s_viol), chk);
	Field(&p, " adjViolCell=", Read(&s_violCell), chk);
	Field(&p, " spanOut=",   Read(&s_spanOut), chk);
	Field(&p, " checks=",    Read(&s_checks), chk);
	Field(&p, " unowned=",   Read(&s_unowned), chk);
	Field(&p, " torn=",      Read(&s_torn), chk);
	Field(&p, " lines=",     Read(&s_violLines), chk);
	FlbStr(&p, "/");         FlbDec(&p, kMaxViolLines);
	LogMsgDeferrable(FlbDone(&p));
}
