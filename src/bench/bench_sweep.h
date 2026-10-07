#pragma once
#include "bench/bench_pin.h"
#include <string>
#include <vector>

// The full sweep: benchmark runs at several slots and speeds, one after the
// other, in stages. No game or Windows calls; the runner is reached only
// through BenchSweepRunner, so the host tests drive it with stubs.

struct BenchSweepLeg
{
	int          slot;        // index into g_benchSlots
	int          speed;       // 0 (paused), 1 or 20
	char         group[16];   // a bench.group name, or "" for the lever A/B
	BenchPinSpec pin;         // the leg's hour and weather; mode BPM_NONE when it names none
};

const int BENCH_SWEEP_MAX_LEGS   = 24;   // per stage
const int BENCH_SWEEP_MAX_STAGES = 8;
const int BENCH_GROUP_TEXT_MAX   = 64;   // bench.group keys kept

// Startup: true when key is bench.sweep; the value, a comma list of
// <slot>:<speed>[:<group>][@<pin>] (spaces trimmed around each entry), becomes
// the leg list. An entry with an unknown slot, a speed other than 0, 1 or 20, a
// group name not of 1-12 a-z0-9 characters or a pin ParseBenchPinSpec refuses
// is appended to *bad and dropped; a
// valid one past BENCH_SWEEP_MAX_LEGS goes to *pastLimit and is dropped. An
// empty value selects the default list; so does one with no valid entry,
// which also sets *usedDefault. Any parse starts the stages over.
bool ParseBenchSweepKey(const std::string& key, const std::string& val, std::vector<std::string>* bad,
                        std::vector<std::string>* pastLimit, bool* usedDefault);

// Startup: true when key is bench.group.<name> or bench.sweep.<n> (n 1-8). A
// group's name (1-12 of a-z0-9) and its value text are stored as given, to be
// resolved by BenchGroupsResolve; a stage's value is parsed as bench.sweep's
// (an entry may add :<group>). A malformed key or entry, or one past a limit,
// is reported through log and dropped; the key still counts as recognised. An
// empty value removes the group or the stage. Any parse starts the stages over.
bool ParseBenchSweepFamilyKey(const std::string& key, const std::string& val,
                              void (*log)(const std::string& line));

int         BenchGroupTextCount();
const char* BenchGroupTextName(int i);
const char* BenchGroupTextValue(int i);
// The bench.group names dropped at startup for the group limit, each once.
int         BenchGroupTextDroppedCount();
const char* BenchGroupTextDroppedName(int i);

// The next stage's legs: the stopped stage while one waits to resume, else the
// stage after the last one done.
int           BenchSweepLegCount();
BenchSweepLeg BenchSweepLegAt(int i);
std::string   BenchSweepListText();   // "swamp:1,swamp:20,city:0:paused,..."

// What the sweep needs from the runner. Main thread only.
struct BenchSweepRunner
{
	// Arms one run at speed without changing the slot; group is a resolved
	// group's index, or -1 for the lever A/B; pin is the leg's; headerExtra is
	// appended to its result header. False with the reason when refused.
	bool (*arm)(int slot, int speed, int group, const BenchPinSpec& pin, const std::string& headerExtra,
	            std::string* whyNot);
	bool (*active)();
	void (*abort)(const char* reason);   // acted on at the runner's next tick
	// NULL when a run can be armed now; otherwise why not, with *isFinal true
	// when waiting cannot help.
	const char* (*armBlocked)(bool* isFinal);
	void (*log)(const std::string& line);
	// The resolved group's index, or -1 with the reason ("unknown", "empty").
	int (*group)(const char* name, const char** whyNot);
};

// Startup, before any other call.
void BenchSweepSetRunner(const BenchSweepRunner& runner);

// From the button: runs the next stage, or resumes a stopped stage at its
// stopped leg. Refuses (one log line) while a sweep is active, when a leg's
// slot is unrecorded or a leg's group does not resolve; stops when the first
// leg's arm is refused. True when a leg is armed.
bool BenchSweepStart();
// From a bench button: the active leg's run is aborted with reason and the
// sweep stops once the runner has ended it.
void BenchSweepAbort(const char* reason);
// The runner's end of every run (its end callback): ok, or the end's reason.
void BenchSweepOnRunEnd(bool ok, const char* reason);
// Every frame on the main thread, after the runner's tick: acts on a leg's
// end and arms the next leg once the runner allows it.
void BenchSweepMainThreadTick(double nowSec);

bool BenchSweepActive();
int  BenchSweepLegNumber();   // 1-based leg of the running stage; 0 when idle or stopping
int  BenchSweepLegTotal();    // legs in the running stage; 0 when idle

// Seconds between one leg ending and the runner allowing the next before the
// sweep gives up on a transient block (a zone transition, a pending restore).
const double BENCH_SWEEP_GAP_LIMIT_SEC = 60.0;
