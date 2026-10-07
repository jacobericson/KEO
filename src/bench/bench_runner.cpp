#include "bench/bench_runner.h"
#include "bench/bench_game.h"
#include "bench/bench_game_math.h"
#include "bench/bench_group.h"
#include "bench/bench_pin.h"
#include "bench/bench_recorders.h"
#include "bench/bench_report.h"
#include "bench/bench_restore.h"
#include "bench/bench_run_report.h"
#include "bench/bench_scenario.h"
#include "bench/bench_slots.h"
#include "base/core.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

namespace bench_runner_detail {

const double SETTLE_LIMIT_SEC = 60.0;
const double SPEED_LIMIT_SEC  = 60.0;
const double REACH_CHECK_SEC  = 0.5;

enum EndKind
{
	END_OK,
	END_ABORT,
	END_USER_SPEED,   // an abort by the user's own speed or pause change: the speed is left alone
	END_REFUSE,
	END_SAVE_LOAD
};

struct Run
{
	bool          active;
	int           slot;
	BenchSlot     target;
	std::string   runId;
	BenchScenario sc;
	size_t        step;
	bool          stepEntered;
	double        stepStart;
	std::string   abortReason;   // set outside the tick, acted on by it
	BenchPinSpec  pin;           // as resolved at arm; mode BPM_NONE when unpinned
	std::string   pinText;       // the header's pin=
	double        posedAt;

	// The user's state, taken when the countdown ends.
	bool         snapped;
	float        userSpeed, userNormalSpeed;
	bool         userPaused;
	BenchPose    userPose;
	bool         userKbd;
	BenchFollow  follow;
	bool         haveFollow;

	// What the run has changed, so an early end restores only that.
	bool settingsChanged, kbdSet, posed, speedSet;
	float holdSpeed;         // the speed KeepSpeed holds: the set speed, 0 once paused

	double clearSince;       // BS_ARM: menus clear since
	int    countdownShown;
	double stableSince;      // BS_SETTLE
	double nextReachCheck;
	int    window;           // current or last window, -1 before the first
	bool   windowOpen;
	bool   measuring;        // the previous frame was in the measured part
	double phaseStart;

	float       hourStart;
	std::string weather;
	int         chars, zones;
};

Run*           s_run           = NULL;   // allocated at the first arm
bool           s_available     = false;
char           s_unavailable[64] = "";  // BenchGameInstall's reason when it refused
bool           s_orderAbort    = false;   // hook_addOrderSelected is installed
bool           s_saveLoading   = false;
bool           s_quitSeen      = false;
LONGLONG       s_lastTick      = 0;
char           s_banner[256]   = "";
void         (*s_onEnd)(bool ok, const char* reason) = NULL;

LONGLONG Qpc()
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

void Logf(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	LogMsg(buf);
}

bool SpeedIs(float speed)
{
	return fabsf(BenchGetSpeed() - speed) <= 0.01f * speed;
}

void ResetRun(Run& r)
{
	r.active = false;
	r.slot = -1;
	r.runId.clear();
	r.sc = BenchScenario();
	r.step = 0;
	r.stepEntered = false;
	r.stepStart = 0.0;
	r.abortReason.clear();
	r.pin = BenchPinNone();
	r.pinText = "none";
	r.posedAt = 0.0;
	r.snapped = false;
	r.userSpeed = r.userNormalSpeed = 1.0f;
	r.userPaused = false;
	r.userKbd = true;
	r.haveFollow = false;
	r.settingsChanged = r.kbdSet = r.posed = r.speedSet = false;
	r.holdSpeed = 1.0f;
	r.clearSince = -1.0;
	r.countdownShown = -1;
	r.stableSince = -1.0;
	r.nextReachCheck = 0.0;
	r.window = -1;
	r.windowOpen = false;
	r.measuring = false;
	r.phaseStart = 0.0;
	r.hourStart = -1.0f;
	r.weather.clear();
	r.chars = r.zones = -1;
}

void Next(Run& r)
{
	++r.step;
	r.stepEntered = false;
}

void WriteReport(const Run& r, const std::string& endReason, bool worldGone, const char* follow)
{
	BenchRunHeader h;
	h.slotKey = BenchSlotKey(r.slot);
	h.speed = r.target.speed;
	h.hourStart = r.hourStart;
	h.hourEndRead = !worldGone;
	h.hourEnd = worldGone ? -1.0f : BenchGetHour();
	h.recordedHour = r.target.hour;
	h.weather = r.weather;
	h.pin = r.pinText;
	h.chars = r.chars;
	h.zones = r.zones;
	h.follow = follow;
	h.orderAbort = s_orderAbort;
	h.banner = s_banner;
	h.qpcFreq = (long long)qpcFrequency.QuadPart;
	BenchReport rep = BuildBenchRunReport(r.sc, r.window + 1, h, r.runId, endReason);
	std::vector<std::string> lines = FormatBenchReport(rep);
	for (size_t i = 0; i < lines.size(); ++i)
		LogMsg(lines[i]);
}

// Every end goes through here: restore what the run changed, log, report.
// After a save load the world is a new one: the scenario's settings are
// put back at once, the speed and keyboard-camera flag once the load is over,
// and the pose and follow not at all. The clock's rate goes back first, at
// once whatever the end: it is not world state.
void EndRun(EndKind kind, const std::string& reason)
{
	BenchPinRelease();
	Run& r = *s_run;
	bool worldGone = kind == END_SAVE_LOAD;

	for (int i = 0; i < r.sc.recorderCount; ++i)
	{
		if (r.windowOpen)
			r.sc.recorders[i]->OnWindowEnd(r.window, !worldGone);
		if (r.snapped)
			r.sc.recorders[i]->OnRunEnd();
	}
	r.windowOpen = false;

	if (r.settingsChanged && r.sc.restoreSettings)
		r.sc.restoreSettings(r.sc.ctx);

	BenchPendingRestore pend = { false, 0.0f, false, false, false };
	if (r.kbdSet)
	{
		if (worldGone)
		{
			pend.kbd = true;
			pend.kbdValue = r.userKbd;
		}
		else
			BenchSetKeyboardCamera(r.userKbd);
	}

	const char* follow = "-";
	if (!worldGone && r.posed)
	{
		BenchSetPose(r.userPose);   // teleport stops any follow, so the follow comes after
		if (!r.haveFollow)
			follow = "off";
		else if (!r.follow.following)
			follow = "none";
		else if (BenchRestoreFollowTarget(r.follow))
			follow = "restored";
		else
		{
			follow = "lost";
			Logf("Bench: camera follow not restored (the target no longer resolves) qpc=%lld", Qpc());
		}
	}

	BenchSpeedEnd se = { false, false, false, 0.0f, 0.0f, 0.0f };
	if (r.speedSet)
		BenchEndSpeed(kind != END_USER_SPEED ? BSE_RESTORE : reason == "unpaused" ? BSE_USER_UNPAUSE : BSE_USER_CHANGE,
		              r.userSpeed, r.userNormalSpeed, r.userPaused, worldGone, s_saveLoading, &pend, &se);
	bool pending = pend.kbd || pend.speed;
	if (pending)
		BenchRestoreQueue(pend);

	LONGLONG q = Qpc();
	if (kind == END_OK)
		Logf("Bench: finished %s qpc=%lld", r.runId.c_str(), q);
	else if (kind == END_REFUSE)
		Logf("Bench: refused %s qpc=%lld", reason.c_str(), q);
	else
		Logf("Bench: aborted %s qpc=%lld", reason.c_str(), q);

	// Refusals are one line; every other end after the snapshot gets a block.
	if (r.snapped && kind != END_REFUSE)
	{
		std::string endReason = reason;
		if (kind != END_OK && r.window >= 0)
		{
			char where[32];
			_snprintf_s(where, sizeof(where), _TRUNCATE, " (window %d)", r.window);
			endReason += where;
		}
		WriteReport(r, endReason, worldGone, follow);
	}

	if (se.unpaused)
		Logf("Bench: unpaused at %.2f (the pause key resumes at %.2f), speed set to the user's %.2f qpc=%lld",
		     se.unpausedAt, se.pauseKey, se.set, Qpc());
	else if (se.resumeSet)
		Logf("Bench: speed left paused, resumes at %.2f qpc=%lld", r.userPaused ? r.userNormalSpeed : r.userSpeed, Qpc());
	else if (se.left)
		Logf("Bench: speed left at the user's choice (%.2f) qpc=%lld", BenchGetSpeed(), Qpc());
	if (pending)
		Logf("Bench: restore pending (%s) qpc=%lld",
		     worldGone ? "save load" : BenchMenusClear() ? "transition" : "menu open", Qpc());
	else if (r.settingsChanged || r.kbdSet || r.posed || r.speedSet)
		Logf("Bench: restored qpc=%lld", Qpc());

	r.active = false;
	r.slot = -1;
	if (s_onEnd)
		s_onEnd(kind == END_OK, reason.c_str());
}

// ---- per-frame pose keeping ----

// Holds the camera on the slot's pose while posed: the keyboard camera off,
// the reach check, the drift reading and a re-apply when it passes the
// limits (teleport traces the ground, so it is not repeated every frame).
// False when the run ended here.
bool KeepPose(Run& r, BenchFrameSample* sample)
{
	BenchSetKeyboardCamera(false);

	double t = ElapsedSec();
	if (t >= r.nextReachCheck)
	{
		r.nextReachCheck = t + REACH_CHECK_SEC;
		float d = BenchNearestPlayerDistance(r.target.pose.pos);
		if (d < 0.0f || d > BENCH_CAMERA_REACH)
		{
			EndRun(END_ABORT, "player character left reach");
			return false;
		}
	}

	BenchPose cur;
	if (!BenchGetPose(&cur))
	{
		EndRun(END_ABORT, "camera lost");
		return false;
	}
	float rot[4] = { r.target.pose.rot[0], r.target.pose.rot[1], r.target.pose.rot[2], r.target.pose.rot[3] };
	BenchNormalizeQuat(rot);
	BenchPoseDrift(r.target.pose.pos, rot, BenchClampZoom(r.target.pose.zoom), cur.pos, cur.rot, cur.zoom,
	               &sample->driftUnits, &sample->driftDeg);
	if (sample->driftUnits > BENCH_DRIFT_UNITS || sample->driftDeg > BENCH_DRIFT_DEG)
		BenchSetPose(r.target.pose);
	return true;
}

// ---- steps ----

// A speed other than the run's is the user's own change, a pause, or the end
// of a pause the run made ("unpaused"): the run ends. False when it ended here.
bool KeepSpeed(Run& r)
{
	if (SpeedIs(r.holdSpeed))
		return true;
	EndRun(END_USER_SPEED, !(BenchGetSpeed() > 0.0f) ? "paused" : r.holdSpeed == 0.0f ? "unpaused" : "speed changed");
	return false;
}

bool Snapshot(Run& r)
{
	if (!BenchGetPose(&r.userPose))
		return false;
	r.userSpeed = BenchGetSpeed();
	r.userPaused = !(r.userSpeed > 0.0f);
	r.userNormalSpeed = BenchGetUserNormalSpeed();
	r.userKbd = BenchGetKeyboardCamera();
	r.haveFollow = BenchGetFollowTarget(&r.follow);
	for (int i = 0; i < r.sc.recorderCount; ++i)
		r.sc.recorders[i]->OnRunStart(r.sc);
	r.snapped = true;
	return true;
}

void TickArm(Run& r)
{
	// Paused (the key or a dialogue) holds the countdown: the snapshot must
	// not take a dialogue's pause for the user's, nor a previous run's speed
	// that is still waiting to be restored for the user's.
	if (BenchRestorePending() || !BenchMenusClear() || !BenchTransitionClear() || !(BenchGetSpeed() > 0.0f))
	{
		r.clearSince = -1.0;
		return;
	}
	double t = ElapsedSec();
	if (r.clearSince < 0.0)
	{
		r.clearSince = t;
		r.countdownShown = -1;
	}
	double left = BENCH_COUNTDOWN_SEC - (t - r.clearSince);
	if (left > 0.0)
	{
		int s = (int)ceil(left);
		if (s != r.countdownShown)
		{
			r.countdownShown = s;
			Logf("Bench: starting in %d s qpc=%lld", s, Qpc());
		}
		return;
	}
	if (!Snapshot(r))
	{
		EndRun(END_REFUSE, "no camera");
		return;
	}
	Logf("Bench: started %s userSpeed=%.2f paused=%d hour=%s qpc=%lld", r.runId.c_str(), r.userSpeed,
	     r.userPaused ? 1 : 0, BenchHourText(BenchGetHour()).c_str(), Qpc());
	Next(r);
}

void TickSetPose(Run& r)
{
	float d = BenchNearestPlayerDistance(r.target.pose.pos);
	if (d < 0.0f || d > BENCH_CAMERA_REACH)
	{
		EndRun(END_REFUSE, "no player character within reach");
		return;
	}
	BenchSetKeyboardCamera(false);
	r.kbdSet = true;
	if (!BenchSetPose(r.target.pose))
	{
		EndRun(END_ABORT, "pose not applied");
		return;
	}
	r.posed = true;
	r.posedAt = ElapsedSec();
	r.nextReachCheck = r.posedAt + REACH_CHECK_SEC;
	Logf("Bench: posed nearest=%.0f qpc=%lld", d, Qpc());
	Next(r);
}

// Speed changes wait for transitions to clear.
void TickSetSpeed(Run& r)
{
	double t = ElapsedSec();
	if (!r.stepEntered)
	{
		r.stepEntered = true;
		r.stepStart = t;
	}
	// A paused run settles at 1x; BS_PAUSE pauses it after the settle.
	int speed = r.target.speed > 1 ? r.target.speed : 1;
	if (BenchMenusClear() && BenchTransitionClear() && BenchSetSpeed((float)speed))
	{
		r.speedSet = true;
		r.holdSpeed = (float)speed;
		Logf("Bench: speed %d qpc=%lld", speed, Qpc());
		Next(r);
		return;
	}
	if (t - r.stepStart > SPEED_LIMIT_SEC)
		EndRun(END_ABORT, "speed change blocked");
}

// The leg's hour and weather, at the run's speed and before the settle; a
// user pause or speed change ends the run at once.
void TickPin(Run& r)
{
	double t = ElapsedSec();
	if (!r.stepEntered)
	{
		r.stepEntered = true;
		BenchPinBegin(r.pin, r.holdSpeed, t);   // when refused, the step ends the run
		r.pinText = BenchPinHeaderText();
	}
	if (!KeepSpeed(r))
		return;
	std::string why;
	int done = BenchPinStep(t, r.posedAt, &why);
	if (done > 0)
		Next(r);
	else if (done < 0)
		EndRun(why.compare(0, 8, "weather ") == 0 || why == "no weather" ? END_REFUSE : END_ABORT, why);
}

// The speed was set running, so the end's restore unpauses to the user's; the
// pause key resumes at the user's speed too, not the run's 1x.
void TickPause(Run& r)
{
	double t = ElapsedSec();
	if (!r.stepEntered)
	{
		r.stepEntered = true;
		r.stepStart = t;
	}
	if (!KeepSpeed(r))
		return;
	if (BenchPause())
	{
		BenchSetPausedResumeSpeed(r.userSpeed);
		r.holdSpeed = 0.0f;
		Logf("Bench: paused qpc=%lld", Qpc());
		Next(r);
		return;
	}
	if (t - r.stepStart > SPEED_LIMIT_SEC)
		EndRun(END_ABORT, "pause refused");
}

void TickSettle(Run& r, const BenchFrameSample& f)
{
	double t = ElapsedSec();
	if (!r.stepEntered)
	{
		r.stepEntered = true;
		r.stepStart = t;
		r.stableSince = -1.0;
		return;
	}
	if (!KeepSpeed(r))
		return;
	bool still = f.driftUnits <= BENCH_DRIFT_UNITS && f.driftDeg <= BENCH_DRIFT_DEG;
	bool ok = still && BenchWorldSettled();
	if (!ok)
		r.stableSince = -1.0;
	else if (r.stableSince < 0.0)
		r.stableSince = t;

	if (r.stableSince >= 0.0 && t - r.stableSince >= BENCH_STABLE_SEC + BENCH_SETTLE_SEC)
	{
		Logf("Bench: settled after %.1f s qpc=%lld", t - r.stepStart, Qpc());
		Next(r);
		return;
	}
	if (t - r.stepStart > SETTLE_LIMIT_SEC)
		EndRun(END_REFUSE, "world did not settle");
}

void TickWindow(Run& r, const BenchStep& step, const BenchFrameSample& f)
{
	const char* setName = r.sc.sets[step.setIndex].c_str();
	double t = ElapsedSec();
	if (!r.stepEntered)
	{
		r.stepEntered = true;
		if (r.window < 0)
		{
			r.hourStart = BenchGetHour();
			r.weather = BenchWeatherText();
			r.chars = BenchPlayerCharacterCount();
			r.zones = BenchLoadedZoneCount();
		}
		++r.window;
		if (r.sc.applySet)
		{
			r.sc.applySet(step.setIndex, r.sc.ctx);
			r.settingsChanged = true;
		}
		for (int i = 0; i < r.sc.recorderCount; ++i)
			r.sc.recorders[i]->OnWindowStart(r.window);
		r.windowOpen = true;
		r.measuring = false;
		r.phaseStart = t;
		Logf("Bench: window %d %s pass%d start qpc=%lld speed=%.2f hour=%s", r.window, setName, step.pass + 1,
		     Qpc(), BenchGetSpeed(), BenchHourText(BenchGetHour()).c_str());
		return;
	}

	if (!KeepSpeed(r))
		return;

	for (int i = 0; i < r.sc.recorderCount; ++i)
		r.sc.recorders[i]->OnFrame(f);
	for (int i = 0; i < r.sc.recorderCount; ++i)
	{
		const char* why = r.sc.recorders[i]->AbortReason();
		if (why)
		{
			EndRun(END_ABORT, why);
			return;
		}
	}

	if (!r.measuring)
	{
		if (t - r.phaseStart >= step.discardSec)
		{
			r.measuring = true;
			r.phaseStart = t;
			Logf("Bench: window %d %s pass%d measure qpc=%lld", r.window, setName, step.pass + 1, Qpc());
		}
		return;
	}
	if (t - r.phaseStart >= step.measureSec)
	{
		std::string fields;
		for (int i = 0; i < r.sc.recorderCount; ++i)
		{
			r.sc.recorders[i]->OnWindowEnd(r.window, true);
			r.sc.recorders[i]->WindowEndFields(r.window, &fields);
		}
		r.windowOpen = false;
		r.measuring = false;
		Logf("Bench: window %d %s pass%d end qpc=%lld%s hour=%s", r.window, setName, step.pass + 1, Qpc(),
		     fields.c_str(), BenchHourText(BenchGetHour()).c_str());
		Next(r);
	}
}

// Why no run can start now, whatever the slot (NULL: one can); *isFinal: waiting cannot help.
const char* StartGate(bool* isFinal)
{
	*isFinal = true;
	if (!s_available)
		return "bench unavailable";
	if (s_quitSeen || g_navMeshStopSeen)
		return "game quitting";
	if (s_saveLoading)
		return "save load in flight";
	*isFinal = false;
	if (!BenchTransitionClear())
		return "no world or a transition in flight";
	return NULL;
}

} // namespace
using namespace bench_runner_detail;

std::string BenchRunnerInstall()
{
	std::string why;
	s_available = BenchGameInstall(&why);
	_snprintf_s(s_unavailable, sizeof(s_unavailable), _TRUNCATE, "%s", s_available ? "" : why.c_str());
	return s_available ? std::string("ok") : "off(" + why + ")";
}

void BenchRunnerSetOrderAbort(bool available)
{
	s_orderAbort = available;
	if (s_available && !available)
		LogMsg("Bench: player-order abort unavailable (movementAware off)");
}

void BenchRunnerSetBanner(const std::string& tokens)
{
	_snprintf_s(s_banner, sizeof(s_banner), _TRUNCATE, "%s", tokens.c_str());
}

bool BenchAvailable()
{
	return s_available;
}

std::string BenchUnavailableReason()
{
	return s_unavailable;
}

bool BenchRunnerActive()
{
	return s_run && s_run->active;
}

int BenchRunnerSlot()
{
	return BenchRunnerActive() ? s_run->slot : -1;
}

void BenchRunnerAbort(const char* reason)
{
	if (BenchRunnerActive() && s_run->abortReason.empty())
		s_run->abortReason = reason && *reason ? reason : "abort";
}

void BenchNotifyPlayerOrder()
{
	BenchRunnerAbort("player order");
}

void BenchRunnerSetEndCallback(void (*onEnd)(bool ok, const char* reason))
{
	s_onEnd = onEnd;
}

const char* BenchRunnerArmBlocked(bool* isFinal)
{
	const char* why = StartGate(isFinal);
	if (why)
		return why;
	if (BenchRunnerActive())
		return "a run is active";
	if (BenchRestorePending())
		return "restore pending";
	return NULL;
}

bool BenchRunnerArm(int slot, int speed, int group, const BenchPinSpec& pin, const std::string& headerExtra,
                    std::string* whyNot)
{
	if (!IsMainThread())
		return false;
	if (BenchRunnerActive())
	{
		BenchRunnerAbort("button");
		if (whyNot)
			*whyNot = "a run is active";
		return false;
	}

	if (!s_run)
		s_run = new Run;
	Run& r = *s_run;
	ResetRun(r);

	bool isFinal = false;
	const char* why = NULL;
	BenchScenarioBuildFn build = NULL;
	BenchScenarioParams params = { BENCH_DISCARD_SEC, BENCH_MEASURE_SEC };
	if (slot < 0 || slot >= BENCH_SLOT_COUNT)
		why = "no such slot";
	else if (speed != 0 && speed != 1 && speed != 20)
		why = "no such speed";
	else if (speed == 0 && group < 0)
		why = "a paused run needs a group";
	else if (!g_benchSlots[slot].recorded)
		why = "slot not recorded";
	else if (group >= 0)
	{
		if (!BuildBenchGroupAB(group, &r.sc, &why))
			why = why ? why : "scenario refused";
	}
	else if (!(build = BenchScenarioBuilder(g_benchSlots[slot].scenario)))
		why = "no such scenario";
	else if (!build(params, &r.sc, &why))
		why = why ? why : "scenario refused";
	if (!why && pin.mode != BPM_NONE && !BenchClockReady())
		why = "clock pin unavailable";
	if (!why && !(why = StartGate(&isFinal)))
	{
		float d = BenchNearestPlayerDistance(g_benchSlots[slot].pose.pos);
		if (d < 0.0f || d > BENCH_CAMERA_REACH)
			why = "no player character within reach";
	}
	if (!why && speed == 0)
		BenchInsertStepAfter(&r.sc, BS_SETTLE, BS_PAUSE);
	if (!why && pin.mode != BPM_NONE)
	{
		BenchPinResolve(pin, g_benchSlots[slot].hour, g_benchSlots[slot].weather, g_benchSlots[slot].weatherStrength, &r.pin);
		r.pinText = FormatBenchPinSpec(r.pin);
		BenchInsertStepAfter(&r.sc, BS_SET_SPEED, BS_PIN);
	}
	if (why)
	{
		Logf("Bench: refused %s qpc=%lld", why, Qpc());
		if (whyNot)
			*whyNot = why;
		return false;
	}

	r.active = true;
	r.slot = slot;
	r.target = g_benchSlots[slot];
	r.target.speed = speed;
	if (!headerExtra.empty())
		r.sc.headerExtra += (r.sc.headerExtra.empty() ? "" : " ") + headerExtra;
	SYSTEMTIME lt;
	GetLocalTime(&lt);
	char id[64];
	_snprintf_s(id, sizeof(id), _TRUNCATE, "%s-%04u%02u%02u-%02u%02u%02u", BenchSlotKey(slot), lt.wYear, lt.wMonth,
	            lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
	r.runId = id;
	Logf("Bench: armed %s speed=%d run=%s qpc=%lld", BenchSlotKey(slot), speed, r.runId.c_str(), Qpc());
	return true;
}

void BenchMainThreadTick(bool saveLoading)
{
	s_saveLoading = saveLoading;
	LONGLONG now = Qpc();
	double frameMs = s_lastTick ? (double)(now - s_lastTick) * 1000.0 / (double)qpcFrequency.QuadPart : 0.0;
	s_lastTick = now;

	// At quit the world is being torn down: drop everything, call nothing in
	// the game; the clock's rate, process memory, goes back.
	if (g_navMeshStopSeen)
	{
		bool wasActive = BenchRunnerActive();
		if (!s_quitSeen && (wasActive || BenchRestorePending()))
			Logf("Bench: run dropped at quit qpc=%lld", now);
		s_quitSeen = true;
		BenchPinRelease();
		BenchRestoreDrop();
		if (s_run)
			s_run->active = false;
		if (wasActive && s_onEnd)
			s_onEnd(false, "game quitting");
		return;
	}

	BenchRestoreTick(saveLoading);
	BenchPinIdleTick();
	if (!BenchRunnerActive())
		return;

	Run& r = *s_run;
	if (saveLoading)
	{
		EndRun(END_SAVE_LOAD, "save load");
		return;
	}
	if (!r.abortReason.empty())
	{
		std::string why = r.abortReason;
		EndRun(END_ABORT, why);
		return;
	}

	// From the pose on, the user's state has been taken: a menu or loading
	// screen ends the run rather than pausing it.
	BenchStepKind kind = r.sc.steps[r.step].kind;
	if (kind != BS_ARM && kind != BS_RESTORE && !BenchMenusClear())
	{
		EndRun(END_ABORT, "menu opened");
		return;
	}

	BenchFrameSample f = { frameMs, 0.0f, 0.0f, r.measuring };
	if (r.posed && !KeepPose(r, &f))
		return;

	const BenchStep& step = r.sc.steps[r.step];
	const char* pinLost = step.kind >= BS_SETTLE && step.kind <= BS_WINDOW ? BenchPinHoldLost() : NULL;
	if (pinLost)
	{
		EndRun(END_ABORT, std::string("pin lost (") + pinLost + ")");
		return;
	}
	switch (step.kind)
	{
	case BS_ARM:      TickArm(r); break;
	case BS_SET_POSE: TickSetPose(r); break;
	case BS_SET_SPEED: TickSetSpeed(r); break;
	case BS_PIN:      TickPin(r); break;
	case BS_SETTLE:   TickSettle(r, f); break;
	case BS_PAUSE:    TickPause(r); break;
	case BS_WINDOW:   TickWindow(r, step, f); break;
	case BS_RESTORE:  EndRun(END_OK, "ok"); break;
	}
}
