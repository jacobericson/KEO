#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

// Pure helpers behind the game facade (bench_game.cpp); no game access.

// The absolute target of a RIP-relative operand: code holds the function's
// bytes from its start (codeAddr), the instruction at insnOff begins with
// opLen opcode bytes followed by a disp32. False when the opcode differs.
bool BenchRipTarget(const unsigned char* code, size_t insnOff, const unsigned char* opcode,
                    size_t opLen, uintptr_t codeAddr, uintptr_t* target);

// Scales q (w, x, y, z) to unit length; false when it is not finite or ~0.
bool BenchNormalizeQuat(float q[4]);

// The camera node's z, which the game keeps negative (behind the centre):
// clamps the distance |zoom| to CameraClass::zoom's 10..2000 and keeps the
// sign; 0 and NaN give -10.
float BenchClampZoom(float zoom);

float BenchDistance3(const float a[3], const float b[3]);

// Angle in degrees between two orientations (w, x, y, z; unit length or
// close to it); q and -q are the same orientation.
float BenchQuatAngleDeg(const float a[4], const float b[4]);

// How far actual is from target: units = the larger of the centre's
// horizontal (x, z) distance and the zoom difference, deg = the orientation
// angle. The centre's height is left out: the game eases it to the ground.
void BenchPoseDrift(const float targetPos[3], const float targetRot[4], float targetZoom,
                    const float pos[3], const float rot[4], float zoom,
                    float* units, float* deg);

// "<name> <strength> wind <speed>", the name with spaces as '_';
// "none" for an empty name.
std::string FormatBenchWeather(const std::string& name, float strength, float wind);

// The speed a run ends at when the user unpauses a pause the run made: the
// speed the user had before the run (the one the pause key resumes at when the
// user was paused), or 1 when that is not positive.
float BenchUnpauseSpeed(float userSpeed, float userNormal, bool userPaused);
