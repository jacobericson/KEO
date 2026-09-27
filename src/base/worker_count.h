#pragma once
// Half the logical processors, clamped to [1, cap].
int AutoNavMeshWorkerCount(int logicalCpus, int cap);
