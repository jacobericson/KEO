#pragma once

struct BenchStats
{
	int    frames;
	double meanMs;
	double low1Ms;   // mean of the slowest 1 % of frames (at least one frame)
	double fps;      // 1000 / meanMs, 0 when there are no frames
};
// ms is not modified; frames with ms <= 0 are skipped.
BenchStats ComputeBenchStats(const float* ms, int n);
