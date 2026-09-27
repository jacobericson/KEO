#include "bench/bench_stats.h"
#include <algorithm>
#include <functional>
#include <vector>

BenchStats ComputeBenchStats(const float* ms, int n)
{
	std::vector<float> v;
	for (int i = 0; i < n; ++i)
		if (ms[i] > 0.0f)
			v.push_back(ms[i]);

	BenchStats s;
	s.frames = (int)v.size();
	s.meanMs = 0.0;
	s.low1Ms = 0.0;
	s.fps = 0.0;
	if (s.frames == 0)
		return s;

	double sum = 0.0;
	for (size_t i = 0; i < v.size(); ++i)
		sum += v[i];
	s.meanMs = sum / v.size();
	s.fps = 1000.0 / s.meanMs;

	std::sort(v.begin(), v.end(), std::greater<float>());
	int lowCount = s.frames / 100;
	if (lowCount < 1)
		lowCount = 1;
	double lowSum = 0.0;
	for (int i = 0; i < lowCount; ++i)
		lowSum += v[i];
	s.low1Ms = lowSum / lowCount;

	return s;
}
