#include "bench/bench_sweep.h"
#include "bench/bench_slots.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bench_sweep_detail {

const BenchSweepLeg kDefaultLegs[] = {
	{ BENCH_SLOT_SWAMP, 1 }, { BENCH_SLOT_SWAMP, 20 }, { BENCH_SLOT_CITY, 1 },
	{ BENCH_SLOT_CITY, 20 }, { BENCH_SLOT_SAND, 1 },   { BENCH_SLOT_SAND, 20 },
};
const int kDefaultLegCount = sizeof(kDefaultLegs) / sizeof(kDefaultLegs[0]);

const int GROUP_NAME_MAX = 12;
const int GROUP_VALUE_MAX = 512;   // the loader's line length bounds a value

// The parsed bench.sweep list; count < 0 means the default.
BenchSweepLeg s_legs[BENCH_SWEEP_MAX_LEGS];
int           s_legCount = -1;

// bench.sweep.<n>, at index n - 1; a count of 0 is a stage key not set.
struct Stage
{
	BenchSweepLeg legs[BENCH_SWEEP_MAX_LEGS];
	int           count;
};
Stage s_stages[BENCH_SWEEP_MAX_STAGES];

struct GroupText
{
	char name[GROUP_NAME_MAX + 1];
	char value[GROUP_VALUE_MAX];
};
GroupText s_groupText[BENCH_GROUP_TEXT_MAX];
int       s_groupTextCount = 0;
char      s_groupDropped[BENCH_GROUP_TEXT_MAX][GROUP_NAME_MAX + 1];   // names past the limit
int       s_groupDroppedCount = 0;

// Progress through the stages: the stage after the last one done, and a
// stopped stage with the leg it stopped at (stage < 0: none).
int  s_nextStage = 0;
int  s_resumeStage = -1;
int  s_resumeLeg = 0;
bool s_ignoredSaid = false;

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

bool GroupNameValid(const std::string& name)
{
	if (name.empty() || name.size() > (size_t)GROUP_NAME_MAX)
		return false;
	for (size_t i = 0; i < name.size(); ++i)
	{
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
}

bool GroupDropped(const char* name)
{
	for (int i = 0; i < s_groupDroppedCount; ++i)
	{
		if (strcmp(s_groupDropped[i], name) == 0)
			return true;
	}
	return false;
}

void ForgetDroppedGroup(const char* name)
{
	for (int i = 0; i < s_groupDroppedCount; ++i)
	{
		if (strcmp(s_groupDropped[i], name) != 0)
			continue;
		for (int j = i; j + 1 < s_groupDroppedCount; ++j)
			memcpy(s_groupDropped[j], s_groupDropped[j + 1], sizeof(s_groupDropped[j]));
		--s_groupDroppedCount;
		return;
	}
}

// <slot>:<speed>[:<group>][@<pin>], speed 0, 1 or 20; the group by its form
// only. *pinWhy: why the pin was refused, else NULL.
bool ParseLeg(const std::string& full, BenchSweepLeg* out, const char** pinWhy)
{
	*pinWhy = NULL;
	BenchPinSpec pin = BenchPinNone();
	size_t at = full.find('@');
	if (at != std::string::npos && !ParseBenchPinSpec(full.substr(at + 1), &pin, pinWhy))
		return false;
	std::string entry = full.substr(0, at);
	size_t colon = entry.find(':');
	if (colon == std::string::npos)
		return false;
	int slot = BenchSlotIndex(Trim(entry.substr(0, colon)));
	std::string rest = entry.substr(colon + 1);
	std::string group;
	size_t colon2 = rest.find(':');
	if (colon2 != std::string::npos)
	{
		group = Trim(rest.substr(colon2 + 1));
		rest = rest.substr(0, colon2);
		if (!GroupNameValid(group))
			return false;
	}
	std::string speed = Trim(rest);
	if (slot < 0 || (speed != "0" && speed != "1" && speed != "20"))
		return false;
	memset(out, 0, sizeof(*out));
	out->slot = slot;
	out->speed = atoi(speed.c_str());
	_snprintf_s(out->group, sizeof(out->group), _TRUNCATE, "%s", group.c_str());
	out->pin = pin;
	return true;
}

// A comma list of legs into out; returns the count kept. badWhy, when given,
// gets one entry per bad one: the pin's refusal, or "".
int ParseLegList(const std::string& t, BenchSweepLeg* out, std::vector<std::string>* bad,
                 std::vector<std::string>* pastLimit, std::vector<std::string>* badWhy = NULL)
{
	int foundCount = 0;
	size_t pos = 0;
	while (pos <= t.size())
	{
		size_t comma = t.find(',', pos);
		std::string entry = Trim(comma == std::string::npos ? t.substr(pos) : t.substr(pos, comma - pos));
		if (!entry.empty())
		{
			BenchSweepLeg leg;
			const char* pinWhy = NULL;
			if (!ParseLeg(entry, &leg, &pinWhy))
			{
				if (bad)
					bad->push_back(entry);
				if (badWhy)
					badWhy->push_back(pinWhy ? pinWhy : "");
			}
			else if (foundCount < BENCH_SWEEP_MAX_LEGS)
				out[foundCount++] = leg;
			else if (pastLimit)
				pastLimit->push_back(entry);
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	return foundCount;
}

void ResetProgress()
{
	s_nextStage = 0;
	s_resumeStage = -1;
	s_resumeLeg = 0;
	s_ignoredSaid = false;
}

bool AnyStageKey()
{
	for (int i = 0; i < BENCH_SWEEP_MAX_STAGES; ++i)
	{
		if (s_stages[i].count > 0)
			return true;
	}
	return false;
}

// The stages in run order: the set stage keys in number order, or bench.sweep alone.
int StageTotal()
{
	int n = 0;
	for (int i = 0; i < BENCH_SWEEP_MAX_STAGES; ++i)
	{
		if (s_stages[i].count > 0)
			++n;
	}
	return n ? n : 1;
}

// The legs of the stage at run position ord.
int StageLegs(int ord, const BenchSweepLeg** legs)
{
	if (!AnyStageKey())
	{
		*legs = s_legCount < 0 ? kDefaultLegs : s_legs;
		return s_legCount < 0 ? kDefaultLegCount : s_legCount;
	}
	for (int i = 0; i < BENCH_SWEEP_MAX_STAGES; ++i)
	{
		if (s_stages[i].count > 0 && ord-- == 0)
		{
			*legs = s_stages[i].legs;
			return s_stages[i].count;
		}
	}
	*legs = kDefaultLegs;
	return 0;
}

// What the next press runs: the stopped stage, else the next one.
int NextStage()
{
	int total = StageTotal();
	if (s_resumeStage >= 0 && s_resumeStage < total)
		return s_resumeStage;
	return s_nextStage < total ? s_nextStage : 0;
}

std::string ListText(const BenchSweepLeg* legs, int n)
{
	std::string out;
	for (int i = 0; i < n; ++i)
	{
		char buf[128];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s%s:%d%s%s", i ? "," : "", BenchSlotKey(legs[i].slot), legs[i].speed,
		            legs[i].group[0] ? ":" : "", legs[i].group);
		out += buf;
		if (legs[i].pin.mode != BPM_NONE)
			out += "@" + FormatBenchPinSpec(legs[i].pin);
	}
	return out;
}

// ---- the sequencer ----

enum Phase
{
	PH_IDLE,
	PH_RUNNING,   // a leg's run is armed or running
	PH_GAP        // the previous leg ended ok; the next waits for the runner
};

BenchSweepRunner s_runner = { NULL, NULL, NULL, NULL, NULL, NULL };
Phase            s_phase = PH_IDLE;
BenchSweepLeg    s_run[BENCH_SWEEP_MAX_LEGS];        // the running stage's legs
int              s_runGroup[BENCH_SWEEP_MAX_LEGS];   // each leg's resolved group, -1 for none
int              s_runCount = 0;
int              s_leg = 0;                          // index of the leg in progress
int              s_stage = 0;                        // run position of the running stage
int              s_stageTotal = 1;
bool             s_stopping = false;                 // an abort was handed to the runner
bool             s_ended = false;                    // the leg's run has ended
bool             s_endOk = false;
char             s_endReason[96] = "";
double           s_gapSince = -1.0;

void Log(const char* fmt, ...)
{
	if (!s_runner.log)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	s_runner.log(buf);
}

// The leg in progress is where the next press resumes.
void Stop(const char* reason)
{
	Log("Bench sweep: stopped at %d/%d (%s), Sweep resumes there", s_leg + 1, s_runCount,
	    reason && *reason ? reason : "unknown");
	s_resumeStage = s_stage;
	s_resumeLeg = s_leg;
	s_phase = PH_IDLE;
	s_stopping = false;
}

// Arms leg s_leg; stops the sweep when the runner refuses.
bool ArmLeg()
{
	const BenchSweepLeg& leg = s_run[s_leg];
	char extra[64];
	_snprintf_s(extra, sizeof(extra), _TRUNCATE, "sweep=%d/%d stage=%d/%d", s_leg + 1, s_runCount, s_stage + 1,
	            s_stageTotal);
	s_ended = false;
	std::string why;
	if (!s_runner.arm(leg.slot, leg.speed, s_runGroup[s_leg], leg.pin, extra, &why))
	{
		Stop(why.c_str());
		return false;
	}
	s_phase = PH_RUNNING;
	Log("Bench sweep: leg %d/%d %s %dx", s_leg + 1, s_runCount, BenchSlotKey(leg.slot), leg.speed);
	return true;
}

// The running stage's last leg ended ok.
void StageDone()
{
	if (s_stage + 1 < s_stageTotal)
	{
		Log("Bench sweep: stage %d/%d done", s_stage + 1, s_stageTotal);
		s_nextStage = s_stage + 1;
	}
	else
	{
		Log("Bench sweep: done %d stages", s_stageTotal);
		s_nextStage = 0;
	}
	s_resumeStage = -1;
	s_resumeLeg = 0;
	s_phase = PH_IDLE;
}

void FamilyLog(void (*log)(const std::string& line), const std::string& line)
{
	if (log)
		log(line);
}

void ParseStageKey(int n, const std::string& key, const std::string& val, void (*log)(const std::string& line))
{
	Stage& st = s_stages[n - 1];
	std::string t = Trim(val);
	if (t.empty())
	{
		st.count = 0;
		return;
	}
	std::vector<std::string> bad, extra, badWhy;
	BenchSweepLeg found[BENCH_SWEEP_MAX_LEGS];
	int count = ParseLegList(t, found, &bad, &extra, &badWhy);
	for (size_t i = 0; i < bad.size(); ++i)
	{
		std::string pinWhy = i < badWhy.size() && !badWhy[i].empty() ? "; pin: " + badWhy[i] : std::string();
		FamilyLog(log, "Bench: " + key + " entry '" + bad[i] +
		               "' ignored (a slot:speed[:group] leg is expected, speed 0, 1 or 20" + pinWhy + ")");
	}
	for (size_t i = 0; i < extra.size(); ++i)
	{
		char buf[32];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", BENCH_SWEEP_MAX_LEGS);
		FamilyLog(log, "Bench: " + key + " entry '" + extra[i] + "' ignored (past the " + buf + "-leg limit)");
	}
	if (count == 0)
		FamilyLog(log, "Bench: " + key + " named no valid leg, ignored");
	st.count = count;
	for (int i = 0; i < count; ++i)
		st.legs[i] = found[i];
}

void ParseGroupKey(const std::string& key, const std::string& name, const std::string& val,
                   void (*log)(const std::string& line))
{
	if (!GroupNameValid(name))
	{
		FamilyLog(log, "Bench: " + key + " ignored (a group name is 1-12 of a-z0-9)");
		return;
	}
	std::string t = Trim(val);
	int at = -1;
	for (int i = 0; i < s_groupTextCount; ++i)
	{
		if (name == s_groupText[i].name)
			at = i;
	}
	if (t.empty())
	{
		ForgetDroppedGroup(name.c_str());
		if (at >= 0)
		{
			for (int i = at; i + 1 < s_groupTextCount; ++i)
				s_groupText[i] = s_groupText[i + 1];
			--s_groupTextCount;
		}
		return;
	}
	if (at < 0)
	{
		if (s_groupTextCount >= BENCH_GROUP_TEXT_MAX)
		{
			char buf[32];
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", BENCH_GROUP_TEXT_MAX);
			FamilyLog(log, "Bench: " + key + " ignored (past the " + buf + "-group limit)");
			if (!GroupDropped(name.c_str()) && s_groupDroppedCount < BENCH_GROUP_TEXT_MAX)
			{
				_snprintf_s(s_groupDropped[s_groupDroppedCount], sizeof(s_groupDropped[0]), _TRUNCATE, "%s",
				            name.c_str());
				++s_groupDroppedCount;
			}
			return;
		}
		at = s_groupTextCount++;
	}
	ForgetDroppedGroup(name.c_str());
	_snprintf_s(s_groupText[at].name, sizeof(s_groupText[at].name), _TRUNCATE, "%s", name.c_str());
	_snprintf_s(s_groupText[at].value, sizeof(s_groupText[at].value), _TRUNCATE, "%s", t.c_str());
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
	ResetProgress();

	std::string t = Trim(val);
	if (t.empty())
	{
		s_legCount = -1;
		return true;
	}

	BenchSweepLeg found[BENCH_SWEEP_MAX_LEGS];
	int foundCount = ParseLegList(t, found, bad, pastLimit);
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

bool ParseBenchSweepFamilyKey(const std::string& key, const std::string& val, void (*log)(const std::string& line))
{
	static const char kGroup[] = "bench.group.";
	static const char kStage[] = "bench.sweep.";
	if (key.compare(0, sizeof(kGroup) - 1, kGroup) == 0)
	{
		ResetProgress();
		ParseGroupKey(key, key.substr(sizeof(kGroup) - 1), val, log);
		return true;
	}
	if (key.compare(0, sizeof(kStage) - 1, kStage) != 0)
		return false;
	ResetProgress();
	std::string num = key.substr(sizeof(kStage) - 1);
	if (num.size() != 1 || num[0] < '1' || num[0] - '0' > BENCH_SWEEP_MAX_STAGES)
	{
		FamilyLog(log, "Bench: " + key + " ignored (the stages are bench.sweep.1 to bench.sweep.8)");
		return true;
	}
	ParseStageKey(num[0] - '0', key, val, log);
	return true;
}

int BenchGroupTextCount()
{
	return s_groupTextCount;
}

const char* BenchGroupTextName(int i)
{
	return i >= 0 && i < s_groupTextCount ? s_groupText[i].name : "";
}

const char* BenchGroupTextValue(int i)
{
	return i >= 0 && i < s_groupTextCount ? s_groupText[i].value : "";
}

int BenchGroupTextDroppedCount()
{
	return s_groupDroppedCount;
}

const char* BenchGroupTextDroppedName(int i)
{
	return i >= 0 && i < s_groupDroppedCount ? s_groupDropped[i] : "";
}

int BenchSweepLegCount()
{
	const BenchSweepLeg* legs = NULL;
	return StageLegs(NextStage(), &legs);
}

BenchSweepLeg BenchSweepLegAt(int i)
{
	const BenchSweepLeg* legs = NULL;
	int n = StageLegs(NextStage(), &legs);
	return i >= 0 && i < n ? legs[i] : kDefaultLegs[0];
}

std::string BenchSweepListText()
{
	const BenchSweepLeg* legs = NULL;
	int n = StageLegs(NextStage(), &legs);
	return ListText(legs, n);
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
	if (AnyStageKey() && s_legCount >= 0 && !s_ignoredSaid)
	{
		Log("Bench sweep: bench.sweep ignored (bench.sweep.<n> keys are set)");
		s_ignoredSaid = true;
	}

	int total = StageTotal();
	int stage = NextStage();
	int first = stage == s_resumeStage ? s_resumeLeg : 0;
	const BenchSweepLeg* legs = NULL;
	int n = StageLegs(stage, &legs);
	if (first >= n)
		first = 0;

	std::string missing;
	bool listed[BENCH_SLOT_COUNT] = { false };
	for (int i = 0; i < n; ++i)
	{
		int slot = legs[i].slot;
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

	int groups[BENCH_SWEEP_MAX_LEGS];
	for (int i = 0; i < n; ++i)
	{
		groups[i] = -1;
		if (!legs[i].group[0])
			continue;
		const char* why = "unknown";
		if (s_runner.group)
			groups[i] = s_runner.group(legs[i].group, &why);
		if (groups[i] < 0)
		{
			char limit[48] = "";
			if (GroupDropped(legs[i].group))
				_snprintf_s(limit, sizeof(limit), _TRUNCATE, " (past the %d-group limit at startup)", BENCH_GROUP_TEXT_MAX);
			Log("Bench sweep: refused (group '%s' %s%s)", legs[i].group, why ? why : "unknown", limit);
			return false;
		}
	}

	for (int i = 0; i < n; ++i)
	{
		s_run[i] = legs[i];
		s_runGroup[i] = groups[i];
	}
	s_runCount = n;
	s_leg = first;
	s_stage = stage;
	s_stageTotal = total;
	s_stopping = false;
	Log("Bench sweep: started stage %d/%d at leg %d/%d %s", stage + 1, total, first + 1, n, ListText(legs, n).c_str());
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
			StageDone();
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
