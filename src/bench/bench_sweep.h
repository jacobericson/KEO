#pragma once
#include <string>
#include <vector>

// The full sweep: lever A/B runs at several slots and speeds, one after the
// other. No game or Windows calls; the runner is reached only through
// BenchSweepRunner, so the host tests drive it with stubs.

struct BenchSweepLeg
{
	int slot;    // index into g_benchSlots
	int speed;   // 1 or 20
};

const int BENCH_SWEEP_MAX_LEGS = 16;

// Startup: true when key is bench.sweep; the value, a comma list of
// <slot>:<speed> (spaces trimmed around each entry), becomes the leg list.
// An entry with an unknown slot or a speed other than 1 or 20 is appended to
// *bad and dropped; a valid one past BENCH_SWEEP_MAX_LEGS goes to *pastLimit
// and is dropped. An empty value selects the default list; so does one with
// no valid entry, which also sets *usedDefault.
bool ParseBenchSweepKey(const std::string& key, const std::string& val, std::vector<std::string>* bad,
                        std::vector<std::string>* pastLimit, bool* usedDefault);

int           BenchSweepLegCount();
BenchSweepLeg BenchSweepLegAt(int i);
std::string   BenchSweepListText();   // "swamp:1,swamp:20,..."

// What the sweep needs from the runner. Main thread only.
struct BenchSweepRunner
{
	// Arms one run at speed without changing the slot; headerExtra is appended
	// to its result header. False with the reason when refused.
	bool (*arm)(int slot, int speed, const std::string& headerExtra, std::string* whyNot);
	bool (*active)();
	void (*abort)(const char* reason);   // acted on at the runner's next tick
	// NULL when a run can be armed now; otherwise why not, with *isFinal true
	// when waiting cannot help.
	const char* (*armBlocked)(bool* isFinal);
	void (*log)(const std::string& line);
};

// Startup, before any other call.
void BenchSweepSetRunner(const BenchSweepRunner& runner);

// From the button: refuses (one log line) while a sweep is active, when a
// leg's slot is unrecorded or when the first leg's arm is refused. True when
// the first leg is armed.
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
int  BenchSweepLegNumber();   // 1-based leg in progress; 0 when idle or stopping
int  BenchSweepLegTotal();    // legs in the active sweep; 0 when idle

// Seconds between one leg ending and the runner allowing the next before the
// sweep gives up on a transient block (a zone transition, a pending restore).
const double BENCH_SWEEP_GAP_LIMIT_SEC = 60.0;
