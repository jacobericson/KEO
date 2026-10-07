// wall_splice.h - A finished or repaired wall's navmesh splice, issued once physics has moved its
// hulls into the gathered group, replacing the splice vanilla queues before the move.
#pragma once

namespace navmesh {

// Main thread, a startup install step: the progress detour while wallSpliceFix is on.
void InstallWallSplice(int* installed, int* total);
// Main thread, at startPlugin after InstallHooks and before any world exists: the call-site NOP,
// written only when the detour installed; arms the pair. Logs one line either way.
void InstallWallSpliceNop(bool gateOk);
// Main thread, every frame from the camera tick: drains, coalesces, gates and issues.
void WallSpliceTick(double now, bool saveLoading);
// A navmesh thread, at a type-1 job's start (DEV): counts a start while the switch is pending.
void WallSpliceNoteType1Start();

} // namespace navmesh
