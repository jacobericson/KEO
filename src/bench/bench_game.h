#pragma once
#include "bench/bench_slots.h"
#include <string>

// The benchmark's only access to the game. Everything is main thread only,
// and every call is a no-op (or returns "unknown") until BenchGameInstall
// has succeeded.

// Resolves and checks every address below once; false with a short reason
// (e.g. "sky", "camera") disables the Benchmark section. Call at startup,
// after the KenshiLib registry; it calls no game function.
bool BenchGameInstall(std::string* whyNot);

bool  BenchGetPose(BenchPose* out);        // false when there is no camera yet
bool  BenchSetPose(const BenchPose& p);    // orientation and zoom, then teleport the centre
// Distance from pos to the nearest player character; negative when unknown.
float BenchNearestPlayerDistance(const float pos[3]);
const float BENCH_CAMERA_REACH = 2250.0f;  // CameraClass::restrictPosition's leash

// No loading screen, escape menu or Options window; false before the camera exists.
bool  BenchMenusClear();
bool  BenchTransitionClear(); // no mod transition, loadingPhase 0, not just loaded
bool  BenchWorldSettled();    // BenchTransitionClear() and not paused

float BenchGetSpeed();                      // frameSpeedMult (0 while paused)
float BenchGetUserNormalSpeed();            // the speed the pause key resumes at
// Both refuse (false, nothing changed) unless BenchMenusClear(): a speed
// above 0 clears any pause, including the escape menu's. BenchSetSpeed also
// refuses 0: setGameSpeed(0) pauses without the pause key's saved speed, so
// the bench pauses only through userPause (BenchRestoreSpeed, BenchPause).
bool  BenchSetSpeed(float speed);           // GameWorld::setGameSpeed(speed, false), speed > 0
// setGameSpeed(speed, false), then userPause(true) when the user was paused,
// so the pause key resumes at speed.
bool  BenchRestoreSpeed(float speed, bool paused);
// The game's own pause, userPause(true): it saves the speed the pause key
// resumes at and zeroes frameSpeedMult; never togglePause alone. False
// (nothing changed) unless BenchMenusClear().
bool  BenchPause();
// While paused: makes the pause key (and anything else calling
// userPause(false)) resume at speed, without unpausing. False when refused.
bool  BenchSetPausedResumeSpeed(float speed);

// The speed the pause key resumes at while paused; negative when unknown.
float BenchGetPausedResumeSpeed();

// The game hour, read here; never written. A pinned leg moves it only forward,
// through the clock's rate. Negative when unknown.
float BenchGetHour();
int   BenchGetDay();                     // the sky's day; negative when unknown

// The clock's rate: one float the sky's per-frame advance multiplies by the
// game speed. Off (every pinned arm refused) when install did not find its
// reader or its shipped value.
bool  BenchClockReady();
float BenchClockDefaultRate();           // the game's own value, read at install
float BenchClockRate();                  // the value now
// Main thread; finite, 0..24. One aligned 4-byte store, read back.
bool  BenchSetClockRate(float rate);

bool  BenchGetKeyboardCamera();              // InputHandler::controlEnabled
void  BenchSetKeyboardCamera(bool enabled);
// The character the camera follows (PlayerInterface::trackedCharacterHandle),
// kept as the handle so it is resolved again at restore.
struct BenchFollow
{
	bool               following;
	unsigned long long handle[4];   // a copy of the game's hand
};
bool  BenchFollowAvailable();                // false: follow is neither read nor restored
bool  BenchGetFollowTarget(BenchFollow* out);
// Starts following f's character again. False (nothing changed) when f was
// not following, follow is unavailable or the character no longer resolves.
bool  BenchRestoreFollowTarget(const BenchFollow& f);

int   BenchPlayerCharacterCount();
int   BenchLoadedZoneCount();                // zones loading or loaded, mod preloads included
std::string BenchWeatherText();              // e.g. "dust 0.40 wind 3.1", or "unknown"
// The camera biome's weather as a pinned leg holds it.
struct BenchWeatherHold { const void* region; const void* weather; float strength; };
bool        BenchWeatherSnapshot(BenchWeatherHold* out);   // the active region's, or false
// Main thread, after the AI join: the active region's current season's weather
// named `name`, set up through the game's own call unless it is the current one,
// then `strength` (0..1; negative: keep the roll's) and its end held ahead of
// the clock. NULL on success, else a reason; *names lists the season's weathers
// on a "not in this region's season" refusal.
const char* BenchForceWeather(const char* name, float strength, std::string* names);
// Main thread, after the AI join: while h is still the active region's weather,
// keeps its end, and its season's end, ahead of the clock so the region does not
// roll another. False (nothing written) when h is no longer current.
bool        BenchWeatherKeep(const BenchWeatherHold& h);

// GetForegroundWindow() belongs to our process. Independent of
// BenchGameInstall: it reads no game state, so it works before install too.
bool  BenchWindowInForeground();
