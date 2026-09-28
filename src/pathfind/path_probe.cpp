// path_probe.cpp - Arm and dump the multi-call path probe.
// Main thread; publication uses Interlocked operations with no diagnostic lock.

#include "pathfind/pathfind_diag.h"

// =========================================================================
// Multi-call path probe: arm + dump
// =========================================================================

void ArmPathProbe()
{
	for (int i = 0; i < PATH_PROBE_SIZE; ++i)
	{
		pathfind::g_pathDiag.pathProbeBuf[i].hookType = 0;
		pathfind::g_pathDiag.pathProbeBuf[i].result = 0;
	}
	InterlockedExchange(&pathfind::g_pathDiag.pathProbeWriteIdx, 0);
	pathfind::g_pathDiag.pathProbeArmTime = ElapsedSec();
	InterlockedExchange(&pathfind::g_pathDiag.pathProbeArmed, 1);
	LogMsg("PathProbe: armed");
}

void DumpPathProbe(double now)
{
	if (InterlockedCompareExchange(&pathfind::g_pathDiag.pathProbeArmed, 0, 0) != 1)
		return;

	if (now - pathfind::g_pathDiag.pathProbeArmTime < 8.0)
		return;

	InterlockedExchange(&pathfind::g_pathDiag.pathProbeArmed, 0);

	long count = InterlockedCompareExchange(&pathfind::g_pathDiag.pathProbeWriteIdx, 0, 0);
	if (count > PATH_PROBE_SIZE)
		count = PATH_PROBE_SIZE;

	{
		std::ostringstream ss;
		ss << "PathProbe: " << count << " entries in "
		   << std::fixed << std::setprecision(1)
		   << (now - pathfind::g_pathDiag.pathProbeArmTime) << "s";
		LogMsg(ss.str());
	}

	for (long i = 0; i < count; ++i)
	{
		const PathProbeEntry& e = pathfind::g_pathDiag.pathProbeBuf[i];
		if (e.hookType == 0 || e.hookType == 3) continue;

		std::ostringstream ss;
		ss << std::fixed << std::setprecision(0);
		ss << "PP[" << i << "] ";

		if (e.hookType == 1)
		{
			ss << "csFP (" << e.startX*10 << "," << e.startZ*10 << ")->("
			   << e.destX*10 << "," << e.destZ*10 << ") f=" << e.faceKey
			   << (e.result ? " OK" : " FAIL");
		}
		else if (e.hookType == 2)
		{
			const char* st = "?";
			if (e.result == 1) st = "OK";
			else if (e.result == 2) st = "UNRCH";
			else if (e.result == 3) st = "TERM";
			else if (e.result == 5) st = "INV";

			unsigned int dFace = 0;
			if (i > 0 && pathfind::g_pathDiag.pathProbeBuf[i-1].hookType == 3)
				dFace = pathfind::g_pathDiag.pathProbeBuf[i-1].faceKey;

			ss << "A* (" << e.startX*10 << "," << e.startZ*10 << ")->("
			   << e.destX*10 << "," << e.destZ*10 << ") sf=" << e.faceKey
			   << " " << st;
			if (dFace) ss << " df=" << dFace;
		}

		LogMsg(ss.str());
	}
}
