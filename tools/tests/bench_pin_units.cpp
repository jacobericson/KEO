#include "bench/bench_pin.h"
#include <cmath>
#include <cstring>

#include "check.h"

static bool Near(float a, float b)
{
	return std::fabs(a - b) <= 1e-4f;
}

static bool Parse(const char* text, BenchPinSpec* out)
{
	const char* why = NULL;
	return ParseBenchPinSpec(text, out, &why);
}

static void AheadTests()
{
	const char* what = "pin: the clock moves forward only, across midnight";
	Check(Near(BenchPinAhead(22.0f, 6.0f), 8.0f), what);
	Check(Near(BenchPinAhead(6.0f, 22.0f), 16.0f), what);
	Check(Near(BenchPinAhead(6.0f, 6.0f), 0.0f), what);
	Check(Near(BenchPinAhead(23.9f, 0.1f), 0.2f), what);
	Check(BenchPinAhead(0.0f, 24.0f) >= 0.0f && BenchPinAhead(0.0f, 24.0f) < 24.0f, what);

	const char* band = "pin: a clock just past the hour is held, one further back is fast-forwarded";
	Check(BenchPinAcceptNow(12.4f, 12.0f), band);
	Check(BenchPinAcceptNow(12.0f, 12.0f) && BenchPinAcceptNow(11.99f, 12.0f), band);
	Check(Near(BenchPinAhead(11.5f, 12.0f), 0.5f) && !BenchPinAcceptNow(11.5f, 12.0f), band);
	Check(Near(BenchPinAhead(12.6f, 12.0f), 23.4f) && !BenchPinAcceptNow(12.6f, 12.0f), band);
	Check(BenchPinAcceptNow(0.2f, 23.9f), band);
}

static void RateTests()
{
	const float game = 0.0091667f;
	const char* fwd = "pin: the rate forwards fast, slows near the hour, and divides by the speed";
	Check(Near(BenchPinRate(BPP_FORWARD, BPM_FREEZE, 1.0f, 8.0f, game), 0.5f), fwd);
	Check(Near(BenchPinRate(BPP_FORWARD, BPM_FREEZE, 1.0f, 0.2f, game), 0.1f), fwd);
	Check(Near(BenchPinRate(BPP_FORWARD, BPM_RUN, 20.0f, 8.0f, game), 0.025f), fwd);
	Check(Near(BenchPinRate(BPP_FORWARD, BPM_PACE, 20.0f, 0.1f, game), 0.005f), fwd);
	Check(Near(BenchPinRate(BPP_FORWARD, BPM_FREEZE, 0.0f, 8.0f, game), 0.5f), fwd);

	const char* hold = "pin: hold is 0 frozen, the game's rate over the speed paced, the game's rate run";
	Check(BenchPinRate(BPP_HOLD, BPM_FREEZE, 20.0f, 0.0f, game) == 0.0f, hold);
	Check(Near(BenchPinRate(BPP_HOLD, BPM_PACE, 20.0f, 0.0f, game), game / 20.0f), hold);
	Check(Near(BenchPinRate(BPP_HOLD, BPM_PACE, 1.0f, 0.0f, game), game), hold);
	Check(Near(BenchPinRate(BPP_HOLD, BPM_RUN, 20.0f, 0.0f, game), game), hold);

	const char* rel = "pin: released is the game's own rate in every mode";
	Check(BenchPinRate(BPP_RELEASED, BPM_FREEZE, 20.0f, 0.0f, game) == game, rel);
	Check(BenchPinRate(BPP_RELEASED, BPM_PACE, 20.0f, 0.0f, game) == game, rel);
	Check(BenchPinRate(BPP_RELEASED, BPM_RUN, 1.0f, 3.0f, game) == game, rel);
	Check(BenchPinRate(BPP_RELEASED, BPM_NONE, 1.0f, 0.0f, game) == game, rel);
}

static void ProgressTests()
{
	const char* what = "pin: progress reaches the hour across midnight and stops";
	BenchPinProgress p;
	BenchPinProgressStart(&p, 22.0f, 2.0f);
	Check(Near(p.ahead, 4.0f) && p.done == 0.0f, what);
	Check(!BenchPinProgressStep(&p, 23.5f), what);
	Check(!BenchPinProgressStep(&p, 0.5f) && Near(p.done, 2.5f), what);
	Check(!BenchPinProgressStep(&p, 1.9f), what);
	Check(BenchPinProgressStep(&p, 2.01f) && Near(p.done, 4.01f), what);
	BenchPinProgressStart(&p, 5.0f, 5.0f);
	Check(BenchPinProgressStep(&p, 5.0f), what);
	BenchPinProgressStart(&p, 5.0f, 6.0f);
	Check(!BenchPinProgressStep(&p, -1.0f) && p.done == 0.0f, what);

	Check(BenchPinHourHeld(12.0f, 12.0005f) && !BenchPinHourHeld(12.0f, 12.01f), "pin: a held hour reads within 0.001");
}

static void ParseTests()
{
	const char* rt = "pin: parse and format round trip";
	BenchPinSpec p, q;
	Check(Parse("22.5", &p) && p.mode == BPM_RUN && !p.rec && Near(p.hour, 22.5f) && !p.weather[0] &&
	      !p.noWeather && p.strength < 0.0f, rt);
	Check(FormatBenchPinSpec(p) == "22.50r" && Parse(FormatBenchPinSpec(p).c_str(), &q) && Near(q.hour, 22.5f) &&
	      q.mode == BPM_RUN, rt);
	Check(Parse("12f/Sand_stream_ambient=0.70", &p) && p.mode == BPM_FREEZE && Near(p.hour, 12.0f) &&
	      strcmp(p.weather, "Sand_stream_ambient") == 0 && Near(p.strength, 0.7f), rt);
	Check(FormatBenchPinSpec(p) == "12.00f/Sand_stream_ambient=0.70", rt);
	Check(Parse("5.8r/-", &p) && p.mode == BPM_RUN && p.noWeather && FormatBenchPinSpec(p) == "5.80r/-", rt);
	Check(Parse("rec", &p) && p.rec && p.mode == BPM_RUN && FormatBenchPinSpec(p) == "recr", rt);
	Check(Parse("recp", &p) && p.rec && p.mode == BPM_PACE && FormatBenchPinSpec(p) == "recp", rt);
	Check(Parse("0", &p) && p.hour == 0.0f && FormatBenchPinSpec(p) == "0.00r", rt);
	Check(Parse("24", &p) && p.hour == 0.0f, rt);
	Check(Parse("12/fog_islands", &p) && p.strength < 0.0f && FormatBenchPinSpec(p) == "12.00r/fog_islands", rt);
	Check(FormatBenchPinSpec(BenchPinNone()) == "none" && BenchPinNone().mode == BPM_NONE, rt);

	const char* every = "pin: a weather is allowed in every mode";
	Check(Parse("6r/fog_islands", &p) && p.mode == BPM_RUN && strcmp(p.weather, "fog_islands") == 0, every);
	Check(Parse("6p/fog_islands=0.5", &p) && p.mode == BPM_PACE && Near(p.strength, 0.5f), every);
	Check(Parse("6f/fog_islands", &p) && p.mode == BPM_FREEZE && Parse("6/fog_islands", &p) && p.mode == BPM_RUN, every);

	const char* bad = "pin: bad hours, modes, names and strengths are refused";
	Check(!Parse("25", &p) && !Parse("-1", &p) && !Parse("12x", &p) && !Parse("12/a b", &p) && !Parse("12/x=1.5", &p),
	      bad);
	Check(!Parse("", &p) && !Parse("12/", &p) && !Parse("12/x=", &p) && !Parse("12.345", &p) && !Parse("re", &p) &&
	      !Parse("12ff", &p) && !Parse("12/x=-0.1", &p) && !Parse(".5", &p), bad);
	Check(!Parse("12/abcdefghijklmnopqrstuvwxyz0123456", &p), bad);
	const char* why = NULL;
	Check(!ParseBenchPinSpec("12x", &p, &why) && why && *why, bad);
}

static void ResolveTests()
{
	const char* what = "pin: rec and the slot's weather resolve at arm";
	BenchPinSpec p, r;
	Parse("rec", &p);
	BenchPinResolve(p, 24.0f, "", -1.0f, &r);
	Check(!r.rec && r.hour == 0.0f && !r.weather[0], what);
	BenchPinResolve(p, 7.25f, "fog_islands", 1.0f, &r);
	Check(Near(r.hour, 7.25f) && strcmp(r.weather, "fog_islands") == 0 && Near(r.strength, 1.0f), what);
	Parse("12/-", &p);
	BenchPinResolve(p, 7.0f, "fog_islands", 1.0f, &r);
	Check(!r.weather[0], what);
	Parse("12/black_desert", &p);
	BenchPinResolve(p, 7.0f, "fog_islands", 1.0f, &r);
	Check(strcmp(r.weather, "black_desert") == 0 && r.strength < 0.0f, what);
	Parse("12r", &p);
	BenchPinResolve(p, 7.0f, "fog_islands", 0.5f, &r);
	Check(r.mode == BPM_RUN && strcmp(r.weather, "fog_islands") == 0 && Near(r.strength, 0.5f), what);
	BenchPinResolve(BenchPinNone(), 7.0f, "fog_islands", 1.0f, &r);
	Check(r.mode == BPM_NONE && !r.weather[0], what);
}

static void NameTests()
{
	const char* what = "pin: weather names match case-insensitively, spaces as underscores";
	Check(BenchWeatherNameMatches("sand_stream_ambient", "Sand stream ambient"), what);
	Check(BenchWeatherNameMatches("Sand_stream_ambient", "Sand_stream_ambient"), what);
	Check(BenchWeatherNameMatches("FOG_ISLANDS", "fog islands"), what);
	Check(!BenchWeatherNameMatches("fog_island", "fog islands") && !BenchWeatherNameMatches("fog_islandsx", "fog islands"),
	      what);
	Check(!BenchWeatherNameMatches("", "") && !BenchWeatherNameMatches(NULL, "x"), what);
	Check(BenchWeatherNameValid("black_desert") && BenchWeatherNameValid("GD-2") && !BenchWeatherNameValid("") &&
	      !BenchWeatherNameValid("a b") && !BenchWeatherNameValid("a=b") &&
	      !BenchWeatherNameValid("abcdefghijklmnopqrstuvwxyz012345"), what);
}

int main()
{
	AheadTests();
	RateTests();
	ProgressTests();
	ParseTests();
	ResolveTests();
	NameTests();
	return CheckExit("bench_pin_units");
}
