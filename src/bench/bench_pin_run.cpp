#include "bench/bench_pin.h"
#include "bench/bench_game.h"
#include "bench/bench_run_report.h"
#include "base/core.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The pin's game side, main thread: the clock forwarded to the leg's hour
// through the clock rate, the mode's rate written, the weather forced, then a
// second of reads that must hold before the run goes on. Every write goes
// through the facade.

namespace bench_pin_run_detail {

enum Phase
{
	PH_IDLE,      // no pin: the rate is the game's, or the tripwire puts it back
	PH_ENTER,     // read the clock, start forwarding or take it as is
	PH_FORWARD,   // the fast rate until the hour is reached
	PH_WEATHER,   // the force, once the pose is a second old
	PH_CHECK,     // one second of reads that must hold
	PH_HELD       // held for the run; BenchPinHoldLost watches it
};

struct PinState
{
	int              phase;
	BenchPinSpec     spec;
	float            speed;
	double           begun;
	BenchPinProgress progress;
	float            wrote;         // the rate the pin last wrote
	float            holdHour;
	int              holdDay;
	bool             weatherHeld;
	BenchWeatherHold weather;
	double           checkSince;
	char             applied[96];   // FormatBenchPinSpec of the last pin begun
};

} // namespace bench_pin_run_detail
using namespace bench_pin_run_detail;

static const double PIN_LIMIT_SEC   = 70.0;   // the fast rate reaches any hour well inside this
static const double WEATHER_POSE_SEC = 1.0;   // the active region is the pose's biome by then
static const double HOLD_CHECK_SEC  = 1.0;

static PinState s_pin;   // main thread only

static long long Qpc()
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

static void Logf(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	LogMsg(buf);
}

static const char* ModeName(int mode)
{
	return mode == BPM_FREEZE ? "freeze" : mode == BPM_PACE ? "pace" : "run";
}

// The mode's rate, once the clock has reached the hour.
static bool WriteHold()
{
	s_pin.wrote = BenchPinRate(BPP_HOLD, s_pin.spec.mode, s_pin.speed, 0.0f, BenchClockDefaultRate());
	return BenchSetClockRate(s_pin.wrote);
}

static bool WriteForward()
{
	float rate = BenchPinRate(BPP_FORWARD, s_pin.spec.mode, s_pin.speed, s_pin.progress.ahead - s_pin.progress.done,
	                          BenchClockDefaultRate());
	if (BenchClockRate() == rate)
		return true;
	s_pin.wrote = rate;
	return BenchSetClockRate(rate);
}

// What no longer holds, or NULL. A held weather's end is kept ahead of the clock.
static const char* Lost()
{
	if (BenchClockRate() != s_pin.wrote)
		return "rate";
	if (s_pin.spec.mode == BPM_FREEZE)
	{
		if (!BenchPinHourHeld(s_pin.holdHour, BenchGetHour()))
			return "hour";
		if (BenchGetDay() != s_pin.holdDay)
			return "day";
	}
	if (s_pin.weatherHeld)
	{
		BenchWeatherHold now;
		if (!BenchWeatherSnapshot(&now) || now.region != s_pin.weather.region)
			return "region";
		if (now.weather != s_pin.weather.weather || now.strength != s_pin.weather.strength)
			return "weather";
		BenchWeatherKeep(s_pin.weather);
	}
	return NULL;
}

bool BenchPinBegin(const BenchPinSpec& spec, float speed, double now)
{
	memset(&s_pin, 0, sizeof(s_pin));
	_snprintf_s(s_pin.applied, sizeof(s_pin.applied), _TRUNCATE, "%s", FormatBenchPinSpec(spec).c_str());
	if (spec.mode == BPM_NONE || !BenchClockReady())
		return false;
	s_pin.phase = PH_ENTER;
	s_pin.spec = spec;
	s_pin.speed = speed > 0.0f ? speed : 1.0f;
	s_pin.begun = now;
	s_pin.wrote = BenchClockDefaultRate();
	return true;
}

int BenchPinStep(double now, double posedAt, std::string* why)
{
	if (s_pin.phase == PH_HELD)
		return 1;
	char buf[192];
	if (s_pin.phase == PH_IDLE)
	{
		*why = "clock pin unavailable";
		return -1;
	}
	if (now - s_pin.begun > PIN_LIMIT_SEC)
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "clock did not reach %.2f", s_pin.spec.hour);
		*why = buf;
		return -1;
	}

	if (s_pin.phase == PH_ENTER)
	{
		float h0 = BenchGetHour();
		if (h0 < 0.0f)
			return 0;
		BenchPinProgressStart(&s_pin.progress, h0, s_pin.spec.hour);
		if (BenchPinAcceptNow(h0, s_pin.spec.hour))
		{
			s_pin.progress.ahead = 0.0f;
			s_pin.phase = PH_WEATHER;
			if (!WriteHold())
				*why = "clock rate not written";
			return why->empty() ? 0 : -1;
		}
		if (!WriteForward())
		{
			*why = "clock rate not written";
			return -1;
		}
		Logf("Bench: clock ff from=%.2f day=%d to=%.2f ahead=%.2fh qpc=%lld", h0, BenchGetDay(), s_pin.spec.hour,
		     s_pin.progress.ahead, Qpc());
		s_pin.phase = PH_FORWARD;
		return 0;
	}

	if (s_pin.phase == PH_FORWARD)
	{
		bool reached = BenchPinProgressStep(&s_pin.progress, BenchGetHour());
		if (!(reached ? WriteHold() : WriteForward()))
		{
			*why = "clock rate not written";
			return -1;
		}
		if (reached)
			s_pin.phase = PH_WEATHER;
		return 0;
	}

	if (s_pin.phase == PH_WEATHER)
	{
		if (s_pin.spec.weather[0])
		{
			if (now - posedAt < WEATHER_POSE_SEC)
				return 0;
			std::string names;
			const char* refused = BenchForceWeather(s_pin.spec.weather, s_pin.spec.strength, &names);
			if (refused)
			{
				*why = refused;
				if (!names.empty())
					*why += " (" + names + ")";
				return -1;
			}
			s_pin.weatherHeld = BenchWeatherSnapshot(&s_pin.weather);
			if (!s_pin.weatherHeld)
			{
				*why = "pin lost (region)";
				return -1;
			}
		}
		s_pin.holdHour = BenchGetHour();
		s_pin.holdDay = BenchGetDay();
		s_pin.checkSince = now;
		s_pin.phase = PH_CHECK;
		return 0;
	}

	const char* lost = Lost();
	if (lost)
	{
		*why = std::string("pin lost (") + lost + ")";
		return -1;
	}
	if (now - s_pin.checkSince < HOLD_CHECK_SEC)
		return 0;
	Logf("Bench: pin target=%.2f hour=%s day=%d mode=%s ff=%.2fh weather=%s qpc=%lld", s_pin.spec.hour,
	     BenchHourText(BenchGetHour()).c_str(), BenchGetDay(), ModeName(s_pin.spec.mode), s_pin.progress.done,
	     BenchWeatherText().c_str(), Qpc());
	s_pin.phase = PH_HELD;
	return 1;
}

const char* BenchPinHoldLost()
{
	return s_pin.phase == PH_HELD ? Lost() : NULL;
}

void BenchPinRelease()
{
	bool held = s_pin.phase != PH_IDLE;
	s_pin.phase = PH_IDLE;
	if (!held || !BenchClockReady() || BenchClockRate() == BenchClockDefaultRate())
		return;
	BenchSetClockRate(BenchClockDefaultRate());
	// No world read while the game is quitting or a save loads.
	bool readable = !g_navMeshStopSeen && BenchTransitionClear();
	Logf("Bench: clock rate restored hour=%s day=%d qpc=%lld", BenchHourText(readable ? BenchGetHour() : -1.0f).c_str(),
	     readable ? BenchGetDay() : -1, Qpc());
}

void BenchPinIdleTick()
{
	if (s_pin.phase != PH_IDLE || !BenchClockReady() || BenchClockRate() == BenchClockDefaultRate())
		return;
	if (BenchSetClockRate(BenchClockDefaultRate()))
		Logf("Bench: clock rate was left changed outside a run, restored qpc=%lld", Qpc());
}

std::string BenchPinHeaderText()
{
	return s_pin.applied[0] ? std::string(s_pin.applied) : std::string("none");
}
