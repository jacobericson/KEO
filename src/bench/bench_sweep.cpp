#include "bench/bench_sweep.h"
#include "bench/bench_slots.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace bench_sweep_detail {

const BenchSweepLeg kDefaultLegs[] = {
	{ BENCH_SLOT_SWAMP, 1 }, { BENCH_SLOT_SWAMP, 20 }, { BENCH_SLOT_CITY, 1 },
	{ BENCH_SLOT_CITY, 20 }, { BENCH_SLOT_SAND, 1 },   { BENCH_SLOT_SAND, 20 },
};
const int kDefaultLegCount = sizeof(kDefaultLegs) / sizeof(kDefaultLegs[0]);

// The parsed list; count < 0 means the default.
BenchSweepLeg s_legs[BENCH_SWEEP_MAX_LEGS];
int           s_legCount = -1;

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

bool ParseLeg(const std::string& entry, BenchSweepLeg* out)
{
	size_t colon = entry.find(':');
	if (colon == std::string::npos)
		return false;
	int slot = BenchSlotIndex(Trim(entry.substr(0, colon)));
	std::string speed = Trim(entry.substr(colon + 1));
	if (slot < 0 || (speed != "1" && speed != "20"))
		return false;
	out->slot = slot;
	out->speed = atoi(speed.c_str());
	return true;
}

// ---- the sequencer ----

enum Phase
{
	PH_IDLE,
	PH_RUNNING,   // a leg's run is armed or running
	PH_GAP        // the previous leg ended ok; the next waits for the runner
};

BenchSweepRunner s_runner = { NULL, NULL, NULL, NULL, NULL };
Phase            s_phase = PH_IDLE;
BenchSweepLeg    s_run[BENCH_SWEEP_MAX_LEGS];   // the active sweep's legs
int              s_runCount = 0;
int              s_leg = 0;                     // index of the leg in progress
bool             s_stopping = false;            // an abort was handed to the runner
bool             s_ended = false;               // the leg's run has ended
bool             s_endOk = false;
char             s_endReason[96] = "";
double           s_gapSince = -1.0;

void Log(const char* fmt, ...)
{
	if (!s_runner.log)
		return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	s_runner.log(buf);
}

void Stop(const char* reason)
{
	Log("Bench sweep: stopped at %d/%d (%s)", s_leg + 1, s_runCount, reason && *reason ? reason : "unknown");
	s_phase = PH_IDLE;
	s_stopping = false;
}

// Arms leg s_leg; stops the sweep when the runner refuses.
bool ArmLeg()
{
	const BenchSweepLeg& leg = s_run[s_leg];
	char extra[32];
	_snprintf_s(extra, sizeof(extra), _TRUNCATE, "sweep=%d/%d", s_leg + 1, s_runCount);
	s_ended = false;
	std::string why;
	if (!s_runner.arm(leg.slot, leg.speed, extra, &why))
	{
		Stop(why.c_str());
		return false;
	}
	s_phase = PH_RUNNING;
	Log("Bench sweep: leg %d/%d %s %dx", s_leg + 1, s_runCount, BenchSlotKey(leg.slot), leg.speed);
	return true;
}

} // namespace
using namespace bench_sweep_detail;

bool ParseBenchSweepKey(const std::string& key, const std::string& val, std::vector<std::string>* bad,
                        std::vector<std::string>* pastLimit, bool* usedDefault)
{
	if (key != "bench.sweep")
		return false;
	if (usedDefault)
		*usedDefault = false;

	std::string t = Trim(val);
	if (t.empty())
	{
		s_legCount = -1;
		return true;
	}

	BenchSweepLeg found[BENCH_SWEEP_MAX_LEGS];
	int foundCount = 0;
	size_t pos = 0;
	while (pos <= t.size())
	{
		size_t comma = t.find(',', pos);
		std::string entry = Trim(comma == std::string::npos ? t.substr(pos) : t.substr(pos, comma - pos));
		if (!entry.empty())
		{
			BenchSweepLeg leg;
			if (!ParseLeg(entry, &leg))
			{
				if (bad)
					bad->push_back(entry);
			}
			else if (foundCount < BENCH_SWEEP_MAX_LEGS)
				found[foundCount++] = leg;
			else if (pastLimit)
				pastLimit->push_back(entry);
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}

	if (foundCount == 0)
	{
		s_legCount = -1;
		if (usedDefault)
			*usedDefault = true;
	}
	else
	{
		s_legCount = foundCount;
		for (int i = 0; i < foundCount; ++i)
			s_legs[i] = found[i];
	}
	return true;
}

int BenchSweepLegCount()
{
	return s_legCount < 0 ? kDefaultLegCount : s_legCount;
}

BenchSweepLeg BenchSweepLegAt(int i)
{
	return s_legCount < 0 ? kDefaultLegs[i] : s_legs[i];
}

std::string BenchSweepListText()
{
	std::string out;
	int n = BenchSweepLegCount();
	for (int i = 0; i < n; ++i)
	{
		BenchSweepLeg leg = BenchSweepLegAt(i);
		char buf[32];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s%s:%d", i ? "," : "", BenchSlotKey(leg.slot), leg.speed);
		out += buf;
	}
	return out;
}

void BenchSweepSetRunner(const BenchSweepRunner& runner)
{
	s_runner = runner;
}

bool BenchSweepStart()
{
	if (!s_runner.arm || !s_runner.active || !s_runner.abort || !s_runner.armBlocked)
		return false;
	if (s_phase != PH_IDLE || s_runner.active())
	{
		Log("Bench sweep: refused (a run is active)");
		return false;
	}

	int n = BenchSweepLegCount();
	std::string missing;
	bool listed[BENCH_SLOT_COUNT] = { false };
	for (int i = 0; i < n; ++i)
	{
		int slot = BenchSweepLegAt(i).slot;
		if (g_benchSlots[slot].recorded || listed[slot])
			continue;
		listed[slot] = true;
		if (!missing.empty())
			missing += ", ";
		missing += BenchSlotKey(slot);
	}
	if (!missing.empty())
	{
		Log("Bench sweep: refused (not recorded: %s)", missing.c_str());
		return false;
	}

	for (int i = 0; i < n; ++i)
		s_run[i] = BenchSweepLegAt(i);
	s_runCount = n;
	s_leg = 0;
	s_stopping = false;
	Log("Bench sweep: started %d legs %s", n, BenchSweepListText().c_str());
	return ArmLeg();
}

void BenchSweepAbort(const char* reason)
{
	if (s_phase == PH_IDLE || s_stopping)
		return;
	if (s_phase == PH_RUNNING && s_runner.active())
	{
		s_stopping = true;
		s_runner.abort(reason);
		return;
	}
	Stop(reason);
}

void BenchSweepOnRunEnd(bool ok, const char* reason)
{
	if (s_phase != PH_RUNNING)
		return;
	s_ended = true;
	s_endOk = ok;
	_snprintf_s(s_endReason, sizeof(s_endReason), _TRUNCATE, "%s", reason ? reason : "");
}

void BenchSweepMainThreadTick(double nowSec)
{
	if (s_phase == PH_RUNNING)
	{
		if (!s_ended)
		{
			if (!s_runner.active())
				Stop("run lost");
			return;
		}
		if (!s_endOk)
		{
			Stop(s_endReason);
			return;
		}
		if (++s_leg >= s_runCount)
		{
			Log("Bench sweep: done %d legs", s_runCount);
			s_phase = PH_IDLE;
			return;
		}
		s_phase = PH_GAP;
		s_gapSince = nowSec;
	}
	if (s_phase != PH_GAP)
		return;

	bool isFinal = false;
	const char* blocked = s_runner.armBlocked(&isFinal);
	if (blocked)
	{
		if (isFinal || nowSec - s_gapSince > BENCH_SWEEP_GAP_LIMIT_SEC)
			Stop(blocked);
		return;
	}
	ArmLeg();
}

bool BenchSweepActive()
{
	return s_phase != PH_IDLE;
}

int BenchSweepLegNumber()
{
	return s_phase == PH_IDLE || s_stopping ? 0 : s_leg + 1;
}

int BenchSweepLegTotal()
{
	return s_phase == PH_IDLE ? 0 : s_runCount;
}
