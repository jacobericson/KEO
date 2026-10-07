#pragma once
#include <string>

// A benchmark leg's time of day and weather. The clock is moved only forward,
// through the game's own clock rate, to the leg's hour; it then runs at the
// game's rate (r, the default), at 1x pace whatever the speed (p) or not at
// all (f). The weather is set up through the game's own call and held for the
// leg. The pure half is host-tested; the game half runs on the main thread.

enum BenchPinMode { BPM_NONE, BPM_FREEZE, BPM_PACE, BPM_RUN };

struct BenchPinSpec
{
	int   mode;          // BenchPinMode; BPM_NONE: the leg is not pinned
	bool  rec;           // the hour is the slot's recorded hour (resolved at arm)
	float hour;          // 0 <= hour < 24
	bool  noWeather;     // "/-": no weather pin, not even the slot's
	char  weather[32];   // "" for none
	float strength;      // 0..1; negative: the roll's
};

BenchPinSpec BenchPinNone();
// "<hour>[f|p|r][/<weather>[=<strength>]]", "rec[f|p|r][/...]", "/-" for no weather. False with a reason.
bool        ParseBenchPinSpec(const std::string& text, BenchPinSpec* out, const char** why);
std::string FormatBenchPinSpec(const BenchPinSpec& p);   // "none" or e.g. "22.50f/Sand_stream_ambient=0.70"
bool        BenchWeatherNameValid(const std::string& name);   // 1-31 of A-Z a-z 0-9 _ -
bool        BenchWeatherNameMatches(const char* want, const std::string& gameName);  // case-insensitive, spaces as _
// The spec a run arms with: rec takes slotHour (24 as 0); a pinned leg that
// names no weather and no "/-" takes the slot's (slotWeather "" for none).
void BenchPinResolve(const BenchPinSpec& in, float slotHour, const char* slotWeather, float slotStrength,
                     BenchPinSpec* out);

// The clock, in game hours.
float BenchPinAhead(float from, float target);         // forward distance, [0, 24)
bool  BenchPinAcceptNow(float from, float target);     // ahead <= 0.02 or 24 - ahead <= 0.5
enum  BenchPinPhase { BPP_FORWARD, BPP_HOLD, BPP_RELEASED };
// The constant the clock should hold: the fast rate while forwarding (0.5 h/s, 0.1 h/s within 0.25 h), divided
// by speed; at hold, 0 (freeze), gameRate / speed (pace) or gameRate (run); released, gameRate.
float BenchPinRate(int phase, int mode, float speed, float remaining, float gameRate);
struct BenchPinProgress { float last; float done; float ahead; };
void  BenchPinProgressStart(BenchPinProgress* p, float from, float target);
bool  BenchPinProgressStep(BenchPinProgress* p, float hourNow);   // true once done >= ahead
bool  BenchPinHourHeld(float pinned, float now);       // |now - pinned| <= 0.001

// The game half, main thread.
bool        BenchPinBegin(const BenchPinSpec& spec, float speed, double now);
int         BenchPinStep(double now, double posedAt, std::string* why);   // -1 failed, 0 wait, 1 done
// NULL while held; "hour", "day", "weather", "region" or "rate". While held, it
// also keeps a pinned weather's end ahead of the clock.
const char* BenchPinHoldLost();
void        BenchPinRelease();        // writes the game's rate back when changed; idempotent
void        BenchPinIdleTick();       // the tripwire while no run holds a pin
std::string BenchPinHeaderText();     // the spec as applied, for pin=
