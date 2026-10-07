#include "bench/bench_pin.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const float FAST_RATE       = 0.5f;    // game hours per real second while forwarding
static const float SLOW_RATE       = 0.1f;    // within SLOW_WITHIN of the hour
static const float SLOW_WITHIN     = 0.25f;
static const float AHEAD_ACCEPT    = 0.02f;   // a clock this close below the hour is taken as is
static const float BEHIND_ACCEPT   = 0.5f;    // and one this far past it, so it never forwards a whole day
static const float HOUR_HELD       = 0.001f;
static const size_t WEATHER_NAME_MAX = 31;

BenchPinSpec BenchPinNone()
{
	BenchPinSpec p;
	memset(&p, 0, sizeof(p));
	p.mode = BPM_NONE;
	p.strength = -1.0f;
	return p;
}

static std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

// "<digits>[.<1-2 digits>]" from text[*pos]; false when absent or malformed.
static bool ParseFixed(const std::string& text, size_t* pos, float* out)
{
	size_t i = *pos;
	double v = 0.0;
	size_t digits = 0;
	while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 6)
	{
		v = v * 10.0 + (text[i++] - '0');
		++digits;
	}
	if (digits == 0)
		return false;
	if (i < text.size() && text[i] == '.')
	{
		++i;
		double scale = 0.1;
		size_t frac = 0;
		while (i < text.size() && text[i] >= '0' && text[i] <= '9')
		{
			if (++frac > 2)
				return false;
			v += (text[i++] - '0') * scale;
			scale *= 0.1;
		}
		if (frac == 0)
			return false;
	}
	*pos = i;
	*out = (float)v;
	return true;
}

static bool Fail(const char** why, const char* reason)
{
	if (why)
		*why = reason;
	return false;
}

bool BenchWeatherNameValid(const std::string& name)
{
	if (name.empty() || name.size() > WEATHER_NAME_MAX)
		return false;
	for (size_t i = 0; i < name.size(); ++i)
	{
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

static char Fold(char c)
{
	if (c == ' ')
		return '_';
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool BenchWeatherNameMatches(const char* want, const std::string& gameName)
{
	if (!want || !*want)
		return false;
	size_t n = strlen(want);
	if (n != gameName.size())
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		if (Fold(want[i]) != Fold(gameName[i]))
			return false;
	}
	return true;
}

bool ParseBenchPinSpec(const std::string& text, BenchPinSpec* out, const char** why)
{
	std::string t = Trim(text);
	BenchPinSpec p = BenchPinNone();
	p.mode = BPM_RUN;
	size_t slash = t.find('/');
	std::string head = t.substr(0, slash);
	size_t pos = 0;
	if (head.compare(0, 3, "rec") == 0)
	{
		p.rec = true;
		pos = 3;
	}
	else
	{
		if (!ParseFixed(head, &pos, &p.hour))
			return Fail(why, "an hour of 0-24 with up to 2 decimals, or rec, is expected");
		if (p.hour > 24.0f)
			return Fail(why, "the hour is past 24");
		if (p.hour >= 24.0f)
			p.hour = 0.0f;
	}
	if (pos < head.size())
	{
		char m = head[pos++];
		if (m == 'f')
			p.mode = BPM_FREEZE;
		else if (m == 'p')
			p.mode = BPM_PACE;
		else if (m == 'r')
			p.mode = BPM_RUN;
		else
			return Fail(why, "the mode is f, p or r");
	}
	if (pos != head.size())
		return Fail(why, "the mode is f, p or r");

	if (slash != std::string::npos)
	{
		std::string w = t.substr(slash + 1);
		if (w == "-")
			p.noWeather = true;
		else
		{
			size_t eq = w.find('=');
			std::string name = w.substr(0, eq);
			if (!BenchWeatherNameValid(name))
				return Fail(why, "a weather name is 1-31 of A-Z a-z 0-9 _ -");
			_snprintf_s(p.weather, sizeof(p.weather), _TRUNCATE, "%s", name.c_str());
			if (eq != std::string::npos)
			{
				std::string s = w.substr(eq + 1);
				size_t sp = 0;
				if (!ParseFixed(s, &sp, &p.strength) || sp != s.size() || p.strength > 1.0f)
					return Fail(why, "a strength is 0-1 with up to 2 decimals");
			}
		}
	}
	*out = p;
	return true;
}

std::string FormatBenchPinSpec(const BenchPinSpec& p)
{
	if (p.mode == BPM_NONE)
		return "none";
	const char mode = p.mode == BPM_FREEZE ? 'f' : p.mode == BPM_PACE ? 'p' : 'r';
	char buf[96];
	if (p.rec)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "rec%c", mode);
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f%c", p.hour, mode);
	std::string out = buf;
	if (p.noWeather)
		out += "/-";
	else if (p.weather[0])
	{
		out += "/";
		out += p.weather;
		if (p.strength >= 0.0f)
		{
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "=%.2f", p.strength);
			out += buf;
		}
	}
	return out;
}

void BenchPinResolve(const BenchPinSpec& in, float slotHour, const char* slotWeather, float slotStrength,
                     BenchPinSpec* out)
{
	BenchPinSpec p = in;
	if (p.mode != BPM_NONE)
	{
		if (p.rec)
		{
			p.rec = false;
			p.hour = slotHour >= 0.0f && slotHour < 24.0f ? slotHour : 0.0f;
		}
		if (!p.noWeather && !p.weather[0] && slotWeather && *slotWeather)
		{
			_snprintf_s(p.weather, sizeof(p.weather), _TRUNCATE, "%s", slotWeather);
			p.strength = slotStrength;
		}
	}
	*out = p;
}

float BenchPinAhead(float from, float target)
{
	float d = fmodf(target - from, 24.0f);
	if (d < 0.0f)
		d += 24.0f;
	if (!(d < 24.0f))
		d = 0.0f;
	return d;
}

bool BenchPinAcceptNow(float from, float target)
{
	float ahead = BenchPinAhead(from, target);
	return ahead <= AHEAD_ACCEPT || 24.0f - ahead <= BEHIND_ACCEPT;
}

float BenchPinRate(int phase, int mode, float speed, float remaining, float gameRate)
{
	if (!(speed > 0.0f))
		speed = 1.0f;
	if (phase == BPP_FORWARD)
		return (remaining > SLOW_WITHIN ? FAST_RATE : SLOW_RATE) / speed;
	if (phase == BPP_HOLD)
	{
		if (mode == BPM_FREEZE)
			return 0.0f;
		if (mode == BPM_PACE)
			return gameRate / speed;
	}
	return gameRate;
}

void BenchPinProgressStart(BenchPinProgress* p, float from, float target)
{
	p->last = from;
	p->done = 0.0f;
	p->ahead = BenchPinAhead(from, target);
}

bool BenchPinProgressStep(BenchPinProgress* p, float hourNow)
{
	if (hourNow >= 0.0f && hourNow <= 24.0f)
	{
		float d = hourNow - p->last;
		if (d < 0.0f)
			d += 24.0f;
		p->done += d;
		p->last = hourNow;
	}
	return p->done >= p->ahead;
}

bool BenchPinHourHeld(float pinned, float now)
{
	return fabsf(now - pinned) <= HOUR_HELD;
}
