#pragma once
#include "base/ini_text.h"
#include <string>
#include <vector>

// A camera pose as the game saves it: the centre node's position and
// orientation (w, x, y, z) and the camera node's z position.
struct BenchPose
{
	float pos[3];
	float rot[4];
	float zoom;     // signed as the game holds it (negative: behind the centre)
};

struct BenchSlot
{
	bool      recorded;   // a valid pose was loaded or recorded
	BenchPose pose;
	float     hour;       // game hour at record time, 0..24
	int       speed;      // game speed for the run: 1 or 20
	int       scenario;   // registered scenario kind; not stored, so 0 (the lever A/B)
};

enum BenchSlotId
{
	BENCH_SLOT_SWAMP,
	BENCH_SLOT_CITY,
	BENCH_SLOT_ROAD,
	BENCH_SLOT_CUSTOM,
	BENCH_SLOT_SAND,
	BENCH_SLOT_COUNT
};

extern BenchSlot g_benchSlots[BENCH_SLOT_COUNT];

const char* BenchSlotKey(int slot);     // "swamp", "city", "road", "custom", "sand"
const char* BenchSlotLabel(int slot);   // "Swamp", "City", "Road", "Custom", "Sand"
int         BenchSlotIndex(const std::string& key);   // the slot whose key this is, else -1

// "x,y,z;w,x,y,z;zoom" with 3 / 6 / 2 decimals.
std::string FormatBenchPose(const BenchPose& p);
// False unless all 8 values parse, are finite, the quaternion's length is
// within 0.9..1.1 and |zoom| is within 10..2000 (the game stores it
// negative; the sign is kept).
bool ParseBenchPose(const std::string& s, BenchPose* out);

// Startup: true when key is bench.<slot>.pose|hour|speed and val parsed;
// the value is stored in slots. A pose that fails to parse leaves the slot
// unrecorded. hour is clamped to 0..24; speed must be 1 or 20.
bool ParseBenchSlotKey(const std::string& key, const std::string& val, BenchSlot* slots);

// The three bench.<slot>.* entries for one slot, all append=true.
void BenchSlotIniEntries(int slot, const BenchSlot& s, std::vector<IniEntry>* out);
