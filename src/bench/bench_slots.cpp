#include "bench/bench_slots.h"
#include <cstdio>
#include <cmath>
#include <float.h>

BenchSlot g_benchSlots[BENCH_SLOT_COUNT];   // zero-initialised static storage

namespace bench_slots_detail {
// In BenchSlotId order.
const char* const kSlotKeys[]   = { "swamp", "city", "road", "custom", "sand" };
const char* const kSlotLabels[] = { "Swamp", "City", "Road", "Custom", "Sand" };
static_assert(sizeof(kSlotKeys) / sizeof(kSlotKeys[0]) == BENCH_SLOT_COUNT, "a key per slot");
static_assert(sizeof(kSlotLabels) / sizeof(kSlotLabels[0]) == BENCH_SLOT_COUNT, "a label per slot");
}
using namespace bench_slots_detail;

const char* BenchSlotKey(int slot)   { return kSlotKeys[slot]; }
const char* BenchSlotLabel(int slot) { return kSlotLabels[slot]; }

int BenchSlotIndex(const std::string& key)
{
	for (int i = 0; i < BENCH_SLOT_COUNT; ++i)
	{
		if (key == kSlotKeys[i])
			return i;
	}
	return -1;
}

std::string FormatBenchPose(const BenchPose& p)
{
	char buf[160];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f,%.3f,%.3f;%.6f,%.6f,%.6f,%.6f;%.2f",
	          p.pos[0], p.pos[1], p.pos[2],
	          p.rot[0], p.rot[1], p.rot[2], p.rot[3],
	          p.zoom);
	return buf;
}

bool ParseBenchPose(const std::string& s, BenchPose* out)
{
	BenchPose p;
	int n = sscanf_s(s.c_str(), "%f,%f,%f;%f,%f,%f,%f;%f",
	                  &p.pos[0], &p.pos[1], &p.pos[2],
	                  &p.rot[0], &p.rot[1], &p.rot[2], &p.rot[3],
	                  &p.zoom);
	if (n != 8)
		return false;
	for (int i = 0; i < 3; ++i)
		if (!_finite(p.pos[i])) return false;
	for (int i = 0; i < 4; ++i)
		if (!_finite(p.rot[i])) return false;
	if (!_finite(p.zoom))
		return false;

	double len = sqrt((double)p.rot[0] * p.rot[0] + (double)p.rot[1] * p.rot[1] +
	                   (double)p.rot[2] * p.rot[2] + (double)p.rot[3] * p.rot[3]);
	if (len < 0.9 || len > 1.1)
		return false;
	float dist = std::fabs(p.zoom);
	if (dist < 10.0f || dist > 2000.0f)
		return false;

	*out = p;
	return true;
}

bool ParseBenchSlotKey(const std::string& key, const std::string& val, BenchSlot* slots)
{
	if (key.compare(0, 6, "bench.") != 0)
		return false;
	size_t dot2 = key.find('.', 6);
	if (dot2 == std::string::npos)
		return false;
	std::string slotName = key.substr(6, dot2 - 6);
	std::string field = key.substr(dot2 + 1);

	int idx = BenchSlotIndex(slotName);
	if (idx < 0)
		return false;

	if (field == "pose")
	{
		BenchPose p;
		if (!ParseBenchPose(val, &p))
			return false;
		slots[idx].pose = p;
		slots[idx].recorded = true;
		return true;
	}
	if (field == "hour")
	{
		float h;
		if (!ParseFloat(val, &h))
			return false;
		if (h < 0.0f) h = 0.0f;
		if (h > 24.0f) h = 24.0f;
		slots[idx].hour = h;
		return true;
	}
	if (field == "speed")
	{
		int v;
		if (!ParseInt(val, &v))
			return false;
		if (v != 1 && v != 20)
			return false;
		slots[idx].speed = v;
		return true;
	}
	return false;
}

void BenchSlotIniEntries(int slot, const BenchSlot& s, std::vector<IniEntry>* out)
{
	std::string prefix = std::string("bench.") + kSlotKeys[slot] + ".";
	char buf[32];

	IniEntry pose;
	pose.key = prefix + "pose";
	pose.value = FormatBenchPose(s.pose);
	pose.kind = INI_TEXT;
	pose.append = true;
	out->push_back(pose);

	IniEntry hour;
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f", s.hour);
	hour.key = prefix + "hour";
	hour.value = buf;
	hour.kind = INI_FLOAT;
	hour.append = true;
	out->push_back(hour);

	IniEntry speed;
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", s.speed);
	speed.key = prefix + "speed";
	speed.value = buf;
	speed.kind = INI_INT;
	speed.append = true;
	out->push_back(speed);
}
