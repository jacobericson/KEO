// splice_seam.h - A navmesh patch box across a cell border: every cell it crosses is regenerated
// in full instead of patched, where the per-cell patches would leave the border closed.
#pragma once

namespace navmesh {

// Main thread, a startup install step after InstallWallSplice: the detour on
// NavMesh::generate(Aabb) while wallSpliceFix, spliceSeamFull and caching are on. Logs one line
// when installed or refused, and one when spliceSeamFull is on but wallSpliceFix or caching is off.
void InstallSpliceSeam(int* installed, int* total);
// Main thread, every frame from the camera tick: acts on the boxes other threads held, and the
// heartbeat.
void SpliceSeamTick(double now, bool saveLoading);

} // namespace navmesh
