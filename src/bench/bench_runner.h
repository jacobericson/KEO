#pragma once
#include <string>

// The benchmark run: one at a time, main thread only, stepped from the
// camera-zone tick.

// Startup, after the render levers: installs the game facade. Returns the
// banner token's value, "ok" or "off(<reason>)".
std::string BenchRunnerInstall();
// Startup, after the hooks: whether hook_addOrderSelected (and so the
// player-order abort) is installed.
void BenchRunnerSetOrderAbort(bool available);
// The banner's render=, gate= and bench= tokens, repeated in every result header.
void BenchRunnerSetBanner(const std::string& tokens);

bool BenchAvailable();                 // BenchRunnerInstall succeeded
std::string BenchUnavailableReason();  // the install's refusal reason; empty when available
// Arms a run of the slot at speed (1 or 20); the slot itself is left
// unchanged. headerExtra, when non-empty, is appended to the result header.
// Refusals are logged and returned in *whyNot; while a run is active it is
// aborted instead ("button") and false is returned.
bool BenchRunnerArm(int slot, int speed, const std::string& headerExtra, std::string* whyNot);
// NULL when BenchRunnerArm could arm now; otherwise why not, with *isFinal
// true when waiting cannot help (bench unavailable, quit, save load).
const char* BenchRunnerArmBlocked(bool* isFinal);
// Called at the end of every armed run, main thread: ok, or the end's reason.
void BenchRunnerSetEndCallback(void (*onEnd)(bool ok, const char* reason));
bool BenchRunnerActive();
int  BenchRunnerSlot();                // -1 when idle
// Aborts and restores on the next tick (never from inside a GUI or game callback).
void BenchRunnerAbort(const char* reason);
void BenchNotifyPlayerOrder();         // a player order aborts a run
void BenchMainThreadTick(bool saveLoading);
