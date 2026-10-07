#include "gui/bench_buttons.h"
#include "gui/settings_rows.h"
#include "bench/bench_game.h"
#include "bench/bench_game_math.h"
#include "bench/bench_runner.h"
#include "bench/bench_slots.h"
#include "bench/bench_sweep.h"
#include "render/render_config.h"
#include "base/core.h"
#include <cstdio>
#include <vector>

// Writes only the slot's bench.<slot>.* keys.
static void SaveSlot(int slot)
{
	std::vector<IniEntry> entries;
	BenchSlotIniEntries(slot, g_benchSlots[slot], &entries);
	SaveIniEntries(entries, "[Bench]", "Bench");
}

static void Record(int slot, int speed)
{
	BenchPose pose;
	if (!BenchGetPose(&pose))
	{
		LogMsg("Bench: record refused (no camera)");
		return;
	}
	pose.zoom = BenchClampZoom(pose.zoom);
	float hour = BenchGetHour();

	BenchSlot& s = g_benchSlots[slot];
	s.pose = pose;
	s.hour = hour < 0.0f ? 0.0f : hour;
	s.speed = speed;
	s.recorded = true;
	SaveSlot(slot);

	char hourText[16];
	if (hour < 0.0f)
		_snprintf_s(hourText, sizeof(hourText), _TRUNCATE, "unknown");
	else
		_snprintf_s(hourText, sizeof(hourText), _TRUNCATE, "%.2f", hour);
	char buf[256];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Bench: recorded %s pose=%s hour=%s speed=%d", BenchSlotKey(slot),
	          FormatBenchPose(pose).c_str(), hourText, speed);
	LogMsg(buf);
}

static void Run(int slot, int speed)
{
	BenchSlot& s = g_benchSlots[slot];
	// An unrecorded slot has no pose to save; the runner refuses it anyway.
	if (s.recorded && s.speed != speed)
	{
		s.speed = speed;
		SaveSlot(slot);
	}
	BenchRunnerArm(slot, speed, -1, std::string(), NULL);
}

int BenchButtonPressed(int id, int speed)
{
	if (!IsMainThread() || !BenchAvailable())
		return -1;
	if (BenchSweepActive())
	{
		BenchSweepAbort("button");
		return -1;
	}
	if (BenchRunnerActive())
	{
		BenchRunnerAbort("button");
		return -1;
	}
	if (id == BENCH_BUTTON_SWEEP)
	{
		BenchSweepStart();
		return BenchRunnerSlot();
	}
	if (speed != 20)
		speed = 1;
	int slot = id % 100;
	if (slot < 0 || slot >= BENCH_SLOT_COUNT)
		return -1;
	if (id - slot == BENCH_BUTTON_RECORD)
		Record(slot, speed);
	else if (id - slot == BENCH_BUTTON_RUN)
		Run(slot, speed);
	return BenchRunnerSlot();
}
