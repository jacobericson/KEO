// nm_quality.cpp — NavMesh generation settings probes + workBuffer probes
// (the game's own settings; nothing here overrides them)

#include "navmesh/generation/nm_quality.h"
#include <stdio.h>   // _snprintf_s (fixed buffer, safe on the NavMesh threads)


static inline double NmBitsToDouble(unsigned int bits)
{
	float f;
	memcpy(&f, &bits, sizeof(f));
	return (double)f;
}

// See nm_quality.h. Every caller holds a work buffer it may read:
// the real one under processJobCS at the first dispatch, or a fresh one whose
// blocks were just copied from the real one.
bool NmCheckGenerationSettingsKey(const char* wb, bool* pruneOkOut, bool* xvOkOut)
{
	bool pruneOk = true;
	bool xvOk = true;
	const NmRegionPruningScalars* rp = NULL;
	if (wb && NmVanillaPruningActive())
	{
		rp = (const NmRegionPruningScalars*)(wb + WB_OFF_REGION_PRUNING);
		pruneOk = NmPruneMatchesVanilla(rp);
		xvOk = NmExtraVertexMatchesVanilla((const NmExtraVertexScalars*)(wb + WB_OFF_EXTRA_VERTEX));
	}
	if (pruneOkOut) *pruneOkOut = pruneOk;
	if (xvOkOut)    *xvOkOut = xvOk;
	if (pruneOk && xvOk)
		return true;

	// Different from the key: L2 off for the session. Only the thread that
	// flips the latch writes the line, so it appears once.
	if (InterlockedCompareExchange(&g_l2Bypass, 1, 0) != 0)
		return false;

	char pruneVals[96];
	_snprintf_s(pruneVals, sizeof(pruneVals), _TRUNCATE, "%g/%g/%g/%d/%d%s",
	            NmBitsToDouble(rp->minRegionAreaBits),
	            NmBitsToDouble(rp->minDistanceToSeedPointsBits),
	            NmBitsToDouble(rp->borderPreservationToleranceBits),
	            (int)rp->preserveVerticalBorderRegions, (int)rp->pruneBeforeTriangulation,
	            pruneOk ? "" : "(DIFF, key 1e+08/0.4/0/0/1)");
	char xvVals[192];
	const NmExtraVertexScalars* xv = (const NmExtraVertexScalars*)(wb + WB_OFF_EXTRA_VERTEX);
	_snprintf_s(xvVals, sizeof(xvVals), _TRUNCATE, "%d/%g/%g/%g/%d/%g/%d/%d/%g/%g/%g%s",
	            (int)xv->vertexSelectionMethod,
	            NmBitsToDouble(xv->vertexFractionBits), NmBitsToDouble(xv->areaFractionBits),
	            NmBitsToDouble(xv->minPartitionAreaBits), xv->numSmoothingIterations,
	            NmBitsToDouble(xv->iterationDampingBits),
	            (int)xv->addVerticesOnBoundaryEdges, (int)xv->addVerticesOnPartitionBorders,
	            NmBitsToDouble(xv->boundaryEdgeSplitLengthBits),
	            NmBitsToDouble(xv->partitionBordersSplitLengthBits),
	            NmBitsToDouble(xv->userVertexOnBoundaryToleranceBits),
	            xvOk ? "" : "(DIFF, key 0/0/0/1000/20/0.05/0/0/50/50/0.001)");
	char line[DEFERRED_LOG_CHARS - 64];   // room for LogMsgDeferrable's tag
	_snprintf_s(line, sizeof(line), _TRUNCATE,
	            "NavMesh L2 bypassed: generation settings differ from the cache key (prune=%s xv=%s)",
	            pruneVals, xvVals);
	LogMsgDeferrable(line);
	return false;
}


static volatile long nmSettingsKeyChecked = 0;

void CheckGenerationSettingsKey(uintptr_t nmg)
{
	// Nothing is copied with pruning off, so the key describes the ctor's own
	// values and there is nothing to check.
	if (!NmVanillaPruningActive())
		return;
	if (InterlockedCompareExchange(&nmSettingsKeyChecked, 1, 0) != 0)
		return;

	uintptr_t wb = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256));
	if (!wb)
	{
		InterlockedExchange(&nmSettingsKeyChecked, 0);   // retry at the next dispatch
		return;
	}

	NmCheckGenerationSettingsKey((const char*)wb, NULL, NULL);
	InterlockedExchange(&nmSettingsKeyChecked, 2);
}

void ProbeNavMeshSettings(uintptr_t nmg)
{
	if (InterlockedCompareExchange(&nmSettingsDumped, 1, 0) != 0)
		return;

	uintptr_t wb = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256));
	if (!wb)
	{
		InterlockedExchange(&nmSettingsDumped, 0);
		return;
	}

	// No CRT strings on bg thread — only float writes + Interlocked
	float* emp = (float*)(wb + 76);  // EdgeMatchingParameters (56 bytes = 14 floats)
	for (int i = 0; i < 14; ++i)
		probeEMP[i] = emp[i];

	float* gen = (float*)(wb + 336);
	for (int i = 0; i < 24; ++i)
		probeGen[i] = gen[i];

	// Havok reflection (2026-09-14): +16 is m_characterHeight and +48 m_quantizationGridSize
	// (m_degenerateAreaThreshold is +60); +472 is userVertices.m_size (m_maxPartitionSize is
	// +372). The DEV log still prints these as quantGrid=/degenArea=/maxPartSize=.
	probeMisc[0] = *(float*)(wb + 16);    // m_characterHeight
	probeMisc[1] = *(float*)(wb + 48);    // m_quantizationGridSize
	probeMisc[2] = *(float*)(wb + 328);   // m_minCharacterWidth
	probeMisc[3] = *(float*)(wb + 400);   // m_boundaryEdgeFilterThreshold
	probeMisc[4] = (float)*(int*)(wb + 72);   // m_maxNumEdgesPerFace
	probeMisc[5] = (float)*(int*)(wb + 132);  // m_edgeMatchingMetric
	probeMisc[6] = (float)*(int*)(wb + 136);  // m_edgeConnectionIterations
	probeMisc[7] = (float)*(int*)(wb + 472);  // userVertices.m_size (logged as maxPartSize=)

	InterlockedExchange(&nmSettingsDumped, 2);
}

// Six fields, read off the real work buffer as the
// game set them (setup_void 0x3C48D0 and the settings ctor 0xDD99D0): expected
// maxSeparation 0.2, cosPlanarAlignment 0.99619, minCorridor 0.4, maxCorridor
// 0.6, minCharacterWidth 0.9, edgeConnectionIterations 2.
void VerifyNavMeshSettings(uintptr_t nmg)
{
	if (InterlockedCompareExchange(&nmSettingsVerified, 1, 0) != 0)
		return;

	uintptr_t wb = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256));
	if (!wb)
	{
		InterlockedExchange(&nmSettingsVerified, 0);
		return;
	}

	float* emp = (float*)(wb + 76);
	verifyEMP[0] = emp[0];                    // m_maxStepHeight (+76)
	verifyEMP[1] = emp[1];                    // m_maxSeparation (+80)
	verifyEMP[2] = emp[4];                    // m_cosPlanarAlignmentAngle (+92)
	verifyEMP[3] = *(float*)(wb + 344);       // m_minCorridorWidth
	verifyEMP[4] = *(float*)(wb + 348);       // m_maxCorridorWidth
	verifyEMP[5] = *(float*)(wb + 328);       // m_minCharacterWidth
	verifyEMP[6] = (float)*(int*)(wb + 136);  // m_edgeConnectionIterations
	InterlockedExchange(&nmSettingsVerified, 2);
}

// SEH-safe probes kept in standalone functions (MSVC 2010 can't mix __try with C++ destructors)
void ProbeWorkBufferSize(uintptr_t nmg)
{
	if (InterlockedCompareExchange(&wbProbeDone, 1, 0) != 0)
		return;

	uintptr_t wb = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_settings, 256));
	uintptr_t sectionMgr = *(uintptr_t*)(KLIB_MEMBER(4, nmg, NavMeshGenerator_navmesh, 240));
	uintptr_t havokObj = 0;
	if (sectionMgr)
		havokObj = *(uintptr_t*)(KLIB_MEMBER(4, sectionMgr, NavMesh_world, 136));

	InterlockedExchange(&probeNMGPtrLo, (long)(nmg & 0xFFFFFFFF));
	InterlockedExchange(&probeNMGPtrHi, (long)((nmg >> 32) & 0xFFFFFFFF));
	InterlockedExchange(&probeWBPtrLo, (long)(wb & 0xFFFFFFFF));
	InterlockedExchange(&probeWBPtrHi, (long)((wb >> 32) & 0xFFFFFFFF));
	InterlockedExchange(&probeHavokPtrLo, (long)(havokObj & 0xFFFFFFFF));
	InterlockedExchange(&probeHavokPtrHi, (long)((havokObj >> 32) & 0xFFFFFFFF));
	InterlockedExchange(&probeWBMatch, (wb != 0 && wb == havokObj) ? 1 : 0);

	if (wb)
	{
		// Scan for access fault boundary (true allocation size)
		int lastNonZero = 0;
		int lastReadable = 0;
		// The fault that ends this scan is the result, not a crash.
		GuardEnter();
		for (int off = 0; off < 65536; off += 8)
		{
			__try
			{
				uintptr_t val = *(uintptr_t*)(wb + off);  // arbitrary readable-memory probe, not an identified array
				lastReadable = off;
				if (val != 0)
					lastNonZero = off;
			}
			__except(EXCEPTION_EXECUTE_HANDLER) { break; }
		}
		GuardLeave();
		InterlockedExchange(&probeWBFieldScan, lastNonZero);
		InterlockedExchange(&probeWBHeapSize, lastReadable);

		InterlockedExchange(&g_workBufAllocSize, lastReadable + 8);

		// hkArray scanner: find all arrays in the workBuffer
		int scanLimit = lastReadable;
		int arrayCount = 0;
		// Same — the scan is expected to run off the end of the buffer.
		GuardEnter();
		for (int off = 0; off + 16 <= scanLimit && arrayCount < WB_MAX_ARRAYS; off += 8)
		{
			__try
			{
				uintptr_t ptr = *(uintptr_t*)(KLIB_MEMBER(4, wb + off, ByteArray_m_data, 0));
				int count     = *(int*)(KLIB_MEMBER(4, wb + off, ByteArray_m_size, 8));
				int capFlags  = *(int*)(KLIB_MEMBER(4, wb + off, ByteArray_m_capacityAndFlags, 12));
				int cap       = capFlags & 0x3FFFFFFF;

				if (ptr > 0x10000 && ptr < 0x7FFFFFFFFFFFULL
				    && count > 0 && count < 100000
				    && cap >= count && cap < 500000)
				{
					wbArrayProbes[arrayCount].offset = off;
					wbArrayProbes[arrayCount].ptr = ptr;
					wbArrayProbes[arrayCount].count = count;
					wbArrayProbes[arrayCount].capFlags = capFlags;
					wbArrayOffsets[arrayCount++] = off;
				}
			}
			__except(EXCEPTION_EXECUTE_HANDLER) { break; }
		}
		GuardLeave();

		// Always include known intermediate arrays (may be zeroed after finalize)
		int knownWritable[] = { 240, 256, 520 };
		for (int k = 0; k < 3; ++k)
		{
			bool found = false;
			for (int a = 0; a < arrayCount; ++a)
			{
				if (wbArrayOffsets[a] == knownWritable[k])
				{ found = true; break; }
			}
			if (!found && arrayCount < WB_MAX_ARRAYS)
				wbArrayOffsets[arrayCount++] = knownWritable[k];
		}

		InterlockedExchange(&wbArrayCount, arrayCount);

		// Classify all detected arrays as writable
		int writableCount = 0;
		for (int a = 0; a < arrayCount && writableCount < WB_MAX_ARRAYS; ++a)
			wbWritableOffsets[writableCount++] = wbArrayOffsets[a];
		InterlockedExchange(&wbWritableCount, writableCount);
	}

	InterlockedExchange(&wbProbeDone, 2);
}

