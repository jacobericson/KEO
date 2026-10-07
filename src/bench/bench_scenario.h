#pragma once
#include <string>
#include <vector>

class BenchRecorder;

enum BenchStepKind
{
	BS_ARM,        // wait for menus and loading to clear, then count down
	BS_SET_POSE,
	BS_SET_SPEED,
	BS_PIN,        // move the clock to the leg's hour and set its weather (pinned runs only)
	BS_SETTLE,     // wait for the world to settle around the pose
	BS_PAUSE,      // pause the game for the windows (runner-owned)
	BS_WINDOW,     // apply a settings set, discard, then measure
	BS_RESTORE
};

struct BenchStep
{
	BenchStepKind kind;
	int   setIndex;     // BS_WINDOW: index into BenchScenario::sets
	int   pass;         // BS_WINDOW: 0 forward, 1 reverse
	float discardSec;   // BS_WINDOW
	float measureSec;   // BS_WINDOW
};

const int BENCH_MAX_RECORDERS = 8;

// The runner's timing, seconds: the countdown once menus clear, the still
// pose before the settle wait, the settle wait, and each window's parts.
const double BENCH_COUNTDOWN_SEC = 3.0;
const double BENCH_STABLE_SEC    = 1.0;
const double BENCH_SETTLE_SEC    = 10.0;
const float  BENCH_DISCARD_SEC   = 3.0f;
const float  BENCH_MEASURE_SEC   = 25.0f;

// What a run steps through. The runner owns the camera, speed, settle and
// restore steps; the scenario owns what a window changes and what is recorded.
struct BenchScenario
{
	std::string              name;
	std::vector<std::string> sets;    // settings-set names, indexed by BenchStep::setIndex
	std::vector<BenchStep>   steps;
	std::string              headerExtra;   // appended to the report header verbatim when non-empty
	// At a window's start: applies set `set`. NULL: the scenario changes no settings.
	void (*applySet)(int set, void* ctx);
	// At the run's end, only after an applySet: puts the user's settings back.
	void (*restoreSettings)(void* ctx);
	void*          ctx;
	BenchRecorder* recorders[BENCH_MAX_RECORDERS];
	int            recorderCount;

	BenchScenario();
	bool AddRecorder(BenchRecorder* r);   // false when full
};

struct BenchScenarioParams
{
	float discardSec;
	float measureSec;
};

// Fills out, or returns false with a reason (a string literal) in *whyNot.
typedef bool (*BenchScenarioBuildFn)(const BenchScenarioParams& p, BenchScenario* out, const char** whyNot);

// Startup, main thread: adds a scenario kind; returns its index (what a
// slot's `scenario` names), or -1 when the table is full.
int BenchRegisterScenario(const char* name, BenchScenarioBuildFn build);
BenchScenarioBuildFn BenchScenarioBuilder(int kind);   // NULL when not registered
const char*          BenchScenarioName(int kind);      // NULL when not registered

BenchStep BenchMakeStep(BenchStepKind kind);
BenchStep BenchMakeWindow(int set, int pass, const BenchScenarioParams& p);
int       BenchWindowCount(const BenchScenario& sc);
// Inserts a kind step right after the first `after` step; nothing when there is none.
void      BenchInsertStepAfter(BenchScenario* sc, BenchStepKind after, BenchStepKind kind);
