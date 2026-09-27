#include "base/worker_count.h"

int AutoNavMeshWorkerCount(int logicalCpus, int cap)
{
	int n = logicalCpus / 2;
	if (n > cap) n = cap;
	if (n < 1) n = 1;
	return n;
}
