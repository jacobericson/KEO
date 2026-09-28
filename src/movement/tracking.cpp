// Movement-aware character tracking
// Watched character management, baseline scanning, active polling,
// and tiered character poll orchestration.

#include "zone/preload/preload.h"
#include "movement/tracking.h"
#include "movement/mover_policy.h"
#include "zone/preload/coverage_stats.h"

// Registry evictions. Defined here, where they happen; the counter line that
// prints it lives in pathfind_hooks.cpp, which not every variant compiles.
// Declared in tracking.h, beside the registry state.
volatile long watchedEvictions = 0;


// =========================================================================
// Character tracking state (defined here, declared in tracking.h)
// =========================================================================

WatchedCharacter watchedChars[MAX_WATCHED];
int numWatched           = 0;
double lastBaselineScan  = 0.0;
double lastActivePoll    = 0.0;
int hookOrderCount       = 0;


// =========================================================================
// Nearest player character (camera focus distance cap)
// =========================================================================

bool FindNearestPlayerCharacterXZ(float px, float pz, float* outX, float* outZ)
{
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf) return false;

	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff   = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 200) return false;

	bool found = false;
	float bestDistSq = 0.0f;
	for (unsigned int i = 0; i < scCount; ++i)
	{
		uintptr_t character = scStuff[i];
		if (!character) continue;

		float cx = GetCharPosX(character);
		float cz = GetCharPosZ(character);
		float dx = cx - px;
		float dz = cz - pz;
		float distSq = dx * dx + dz * dz;
		if (!found || distSq < bestDistSq)
		{
			found = true;
			bestDistSq = distSq;
			*outX = cx;
			*outZ = cz;
		}
	}
	return found;
}


// =========================================================================
// Watched character helpers
// =========================================================================

bool AddWatchedCharacter(uintptr_t character, uintptr_t charMovement,
                         int destZX, int destZY, int curZX, int curZY,
                         bool hasMoveOrder)
{
	for (int i = 0; i < numWatched; ++i)
	{
		if (watchedChars[i].character == character)
		{
			watchedChars[i].destZoneX = destZX;
			watchedChars[i].destZoneY = destZY;
			watchedChars[i].currentZoneX = curZX;
			watchedChars[i].currentZoneY = curZY;
			// Upgrade-only: false -> true allowed, true -> false ignored.
			// Return true on upgrade so the caller counts the state change
			// (drives "Order captured" log + immediate reprio trigger).
			// Also run the directional next-zone preload seed so upgraded
			// chars get the same cheap heuristic as fresh adds.
			if (hasMoveOrder && !watchedChars[i].hasMoveOrder)
			{
				watchedChars[i].hasMoveOrder = true;
				int dirX = 0, dirY = 0;
				if (destZX > curZX) dirX = 1; else if (destZX < curZX) dirX = -1;
				if (destZY > curZY) dirY = 1; else if (destZY < curZY) dirY = -1;
				int enqueued = 0;
				if (dirX != 0 && EnqueueCharacterZone(curZX + dirX, curZY)) enqueued++;
				if (dirY != 0 && EnqueueCharacterZone(curZX, curZY + dirY)) enqueued++;
				if (dirX != 0 && dirY != 0 && EnqueueCharacterZone(curZX + dirX, curZY + dirY)) enqueued++;
				if (enqueued > 0)
				{
					std::ostringstream ss;
					ss << "Next-zone preload (upgrade): char at (" << curZX << "," << curZY
					   << ") dir=(" << dirX << "," << dirY << ") enqueued " << enqueued << " zones";
					LogMsg(ss.str());
				}
				return true;
			}
			return false;
		}
	}

	if (numWatched >= WatchedCapacity(playerCharRegistryEnabled))
	{
		// The oldest entry with no move order may be overwritten in place; an
		// entry carrying one never is, so a full registry drops the add.
		MoverSlot slots[MAX_WATCHED];
		for (int i = 0; i < numWatched; ++i)
		{
			slots[i].hasMoveOrder = watchedChars[i].hasMoveOrder;
			slots[i].addedTime    = watchedChars[i].addedTime;
		}
		int evictIdx = playerCharRegistryEnabled
		             ? ChooseWatchedEvictSlot(slots, numWatched) : -1;
		if (evictIdx >= 0)
		{
			// Every field is written: the slot still holds a stale character.
			watchedChars[evictIdx].character    = character;
			watchedChars[evictIdx].charMovement = charMovement;
			watchedChars[evictIdx].hasMoveOrder = hasMoveOrder;
			watchedChars[evictIdx].destZoneX    = destZX;
			watchedChars[evictIdx].destZoneY    = destZY;
			watchedChars[evictIdx].currentZoneX = curZX;
			watchedChars[evictIdx].currentZoneY = curZY;
			watchedChars[evictIdx].addedTime    = ElapsedSec();

			InterlockedIncrement(&watchedEvictions);

			if (hasMoveOrder)
			{
				int dirX = 0, dirY = 0;
				if (destZX > curZX) dirX = 1; else if (destZX < curZX) dirX = -1;
				if (destZY > curZY) dirY = 1; else if (destZY < curZY) dirY = -1;
				if (dirX != 0) EnqueueCharacterZone(curZX + dirX, curZY);
				if (dirY != 0) EnqueueCharacterZone(curZX, curZY + dirY);
				if (dirX != 0 && dirY != 0) EnqueueCharacterZone(curZX + dirX, curZY + dirY);
			}
			return true;
		}

		if (hasMoveOrder)
		{
			static double lastFullWarn = 0.0;
			double t = ElapsedSec();
			if (t - lastFullWarn > 5.0)
			{
				LogMsg("WARN watchedChars full, no evictable entry - dropping move-order add");
				lastFullWarn = t;
			}
		}
		return false;
	}

	watchedChars[numWatched].character = character;
	watchedChars[numWatched].charMovement = charMovement;
	watchedChars[numWatched].hasMoveOrder = hasMoveOrder;
	watchedChars[numWatched].destZoneX = destZX;
	watchedChars[numWatched].destZoneY = destZY;
	watchedChars[numWatched].currentZoneX = curZX;
	watchedChars[numWatched].currentZoneY = curZY;
	watchedChars[numWatched].addedTime = ElapsedSec();
	numWatched++;

	// Immediately preload next zone(s) in path direction.
	// Ensures the zone ahead is always in the preload queue
	// regardless of edge detection threshold. Only fires for move-order adds;
	// baseline adds (curZ==destZ) are no-ops here anyway.
	if (hasMoveOrder)
	{
		int dirX = 0, dirY = 0;
		if (destZX > curZX) dirX = 1; else if (destZX < curZX) dirX = -1;
		if (destZY > curZY) dirY = 1; else if (destZY < curZY) dirY = -1;
		int enqueued = 0;
		if (dirX != 0 && EnqueueCharacterZone(curZX + dirX, curZY)) enqueued++;
		if (dirY != 0 && EnqueueCharacterZone(curZX, curZY + dirY)) enqueued++;
		if (dirX != 0 && dirY != 0 && EnqueueCharacterZone(curZX + dirX, curZY + dirY)) enqueued++;
		if (enqueued > 0)
		{
			std::ostringstream ss;
			ss << "Next-zone preload: char at (" << curZX << "," << curZY
			   << ") dir=(" << dirX << "," << dirY << ") enqueued " << enqueued << " zones";
			LogMsg(ss.str());
		}
	}

	return true;
}

bool IsCharacterWatched(uintptr_t character)
{
	for (int i = 0; i < numWatched; ++i)
	{
		if (watchedChars[i].character == character)
			return true;
	}
	return false;
}

bool IsCharacterMovingOnOrder(uintptr_t character)
{
	for (int i = 0; i < numWatched; ++i)
	{
		if (watchedChars[i].character == character)
			return watchedChars[i].hasMoveOrder;
	}
	return false;
}

void EnsurePlayerCharsWatched()
{
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf) return;

	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff   = GetPlayerCharStuff(playerIntf);
	if (!scStuff || scCount == 0 || scCount > 200) return;

	int added = 0;
	for (unsigned int i = 0; i < scCount; ++i)
	{
		uintptr_t character = scStuff[i];
		if (!character) continue;
		if (IsCharacterWatched(character)) continue;  // upgrade-only contract

		float charX = GetCharPosX(character);
		float charZ = GetCharPosZ(character);
		int gx, gy;
		if (!WorldToZoneGrid(charX, charZ, &gx, &gy)) continue;

		uintptr_t charMov = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
		if (!charMov) continue;

		// destZ == curZ for baseline (no move order)
		if (AddWatchedCharacter(character, charMov, gx, gy, gx, gy, /*hasMoveOrder=*/false))
			added++;
	}

	if (added > 0)
	{
		std::ostringstream ss;
		ss << "Baseline watch: +" << added << " player chars (total " << numWatched << ")";
		LogDebug(ss.str());
	}
}

void RemoveWatchedCharacter(int index)
{
	if (index < 0 || index >= numWatched)
		return;
	numWatched--;
	if (index < numWatched)
		watchedChars[index] = watchedChars[numWatched];
	watchedChars[numWatched].character = 0;
	watchedChars[numWatched].charMovement = 0;
	watchedChars[numWatched].hasMoveOrder = false;
}


// =========================================================================
// Character scanning (baseline: current-position preloading only)
// =========================================================================

void ScanCharacterZones(void* zoneMgr)
{
	if (playerCharRegistryEnabled)
		EnsurePlayerCharsWatched();

	// Per-character grid scan: one 3x3 (or 2x2, when the preload budget is
	// tight) around every player character that is not already following a
	// move order. The registry watches stationary characters too, so the
	// skip below tests the move order, not mere membership -- skipping every
	// watched character would leave a full registry with no grid coverage
	// at all.
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	if (!playerIntf)
		return;

	struct ZoneCenter { int gx, gy; };
	ZoneCenter centers[MAX_CHAR_ZONES];
	int numCenters = 0;
	int charCount = 0;
	int centersDropped = 0;   // cells past MAX_CHAR_ZONES, which get no grid

	unsigned int scCount = GetPlayerCharCount(playerIntf);
	uintptr_t* scStuff = GetPlayerCharStuff(playerIntf);

	if (scStuff && scCount > 0 && scCount <= 200)
	{
		for (unsigned int i = 0; i < scCount; ++i)
		{
			uintptr_t character = scStuff[i];
			if (!character)
				continue;

			// Skip characters PollActiveMovers already preloads ahead of.
			if (numWatched > 0 && IsCharacterMovingOnOrder(character))
				continue;

			float charX = GetCharPosX(character);
			float charZ = GetCharPosZ(character);

			int gx, gy;
			if (WorldToZoneGrid(charX, charZ, &gx, &gy))
			{
				bool found = false;
				for (int c = 0; c < numCenters; ++c)
				{
					if (centers[c].gx == gx && centers[c].gy == gy)
					{ found = true; break; }
				}
				if (!found)
				{
					if (numCenters < MAX_CHAR_ZONES)
					{
						centers[numCenters].gx = gx;
						centers[numCenters].gy = gy;
						numCenters++;
					}
					else
					{
						// A cell past the cap gets no grid at all. Silent here
						// would look exactly like a roster that happens to fit.
						centersDropped++;
					}
				}
			}
			charCount++;
		}
	}

	int zonesEnqueued = 0;

	// The 2x2 fallback is a live clamp, not a leftover: charQueue is still
	// MAX_PRELOADED entries and EnqueueCharacterZone still refuses past it,
	// so a full 3x3 per center is only affordable while the budget divides
	// that way. The same predicate feeds the log below, so the line can
	// never disagree with the grid that actually ran.
	bool useFullGrid = CoverageFullGridAllowed(MAX_PRELOADED, CAMERA_RESERVED, numCenters);

	if (numCenters > 0)
	{
		for (int c = 0; c < numCenters; ++c)
		{
			int cx = centers[c].gx;
			int cy = centers[c].gy;

			if (useFullGrid)
			{
				for (int i = 0; i < 9; ++i)
				{
					if (EnqueueCharacterZone(cx + ORDER_DX[i], cy + ORDER_DY[i]))
						zonesEnqueued++;
				}
			}
			else
			{
				for (int i = 0; i < 4; ++i)
				{
					if (EnqueueCharacterZone(cx + SMALL_DX[i], cy + SMALL_DY[i]))
						zonesEnqueued++;
				}
			}
		}
	}

	charZonesQueued += zonesEnqueued;
	CoverageNoteCharScan(numCenters, useFullGrid, zonesEnqueued);
	CoverageNoteCentersDropped(centersDropped);

	std::ostringstream ss;
	ss << "Character scan: " << charCount << " chars"
	   << ", " << numCenters << " centers"
	   << (useFullGrid ? " x3x3" : " x2x2")
	   << ", " << zonesEnqueued << " new zones queued"
	   << (centersDropped > 0 ? " OVERFLOW" : "");
	LogDebug(ss.str());
}


// =========================================================================
// Tiered character polling
// =========================================================================

void PollActiveMovers(void* zoneMgr, double now)
{
	int removals = 0;
	int edgePreloads = 0;

	// MH2: read playerCharacters lektor once for validation
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	unsigned int scCount = 0;
	uintptr_t* scStuff = NULL;
	if (playerIntf)
	{
		scCount = GetPlayerCharCount(playerIntf);
		scStuff = GetPlayerCharStuff(playerIntf);
	}

	for (int i = numWatched - 1; i >= 0; --i)
	{
		uintptr_t character = watchedChars[i].character;
		uintptr_t charMov   = watchedChars[i].charMovement;

		if (!character || !charMov)
		{
			RemoveWatchedCharacter(i);
			removals++;
			continue;
		}

		// MH2: validate character is still alive before reading offsets
		bool stillAlive = false;
		if (scStuff && scCount > 0 && scCount <= 200)
		{
			for (unsigned int j = 0; j < scCount; ++j)
			{
				if (scStuff[j] == character) { stillAlive = true; break; }
			}
		}
		if (!stillAlive)
		{
			RemoveWatchedCharacter(i);
			removals++;
			continue;
		}

		uintptr_t pathObj = *(uintptr_t*)(KLIB_MEMBER(3, charMov, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
		if (!pathObj)
		{
			RemoveWatchedCharacter(i);
			removals++;
			continue;
		}

		float curX = GetCharPosX(character);
		float curZ = GetCharPosZ(character);

		int curGX, curGY;
		if (!WorldToZoneGrid(curX, curZ, &curGX, &curGY))
		{
			RemoveWatchedCharacter(i);
			removals++;
			continue;
		}

		// Baseline chars: align dest with cur so the arrival check below always
		// fires, keeping them on the early-continue path. Without this, a
		// baseline char whose AI moved it cross-zone since the last 5s scan
		// would fall through to edge-detection and burst-preload it does not
		// need. Move-order chars keep their stored dest so the
		// arrival check fires meaningfully on real arrival.
		if (!watchedChars[i].hasMoveOrder)
		{
			watchedChars[i].destZoneX = curGX;
			watchedChars[i].destZoneY = curGY;
		}

		if (curGX == watchedChars[i].destZoneX && curGY == watchedChars[i].destZoneY)
		{
			// Arrived. With the registry on the entry is downgraded to a
			// baseline one and the scan keeps owning its lifetime, so the
			// zone it stands in keeps ranking; without it, it is dropped.
			if (playerCharRegistryEnabled)
			{
				if (watchedChars[i].hasMoveOrder)
				{
					watchedChars[i].hasMoveOrder = false;
					watchedChars[i].destZoneX = curGX;
					watchedChars[i].destZoneY = curGY;
				}
				continue;
			}
			RemoveWatchedCharacter(i);
			removals++;
			continue;
		}

		watchedChars[i].currentZoneX = curGX;
		watchedChars[i].currentZoneY = curGY;

		// Always ensure next zone in path is preloaded (no edge threshold).
		// Characters must always have at least their current zone and next
		// zone ahead available for pathfinding.
		{
			int dirX = 0, dirY = 0;
			if (watchedChars[i].destZoneX > curGX) dirX = 1;
			else if (watchedChars[i].destZoneX < curGX) dirX = -1;
			if (watchedChars[i].destZoneY > curGY) dirY = 1;
			else if (watchedChars[i].destZoneY < curGY) dirY = -1;
			if (dirX != 0 && EnqueueCharacterZone(curGX + dirX, curGY)) edgePreloads++;
			if (dirY != 0 && EnqueueCharacterZone(curGX, curGY + dirY)) edgePreloads++;
			if (dirX != 0 && dirY != 0 && EnqueueCharacterZone(curGX + dirX, curGY + dirY)) edgePreloads++;
		}

		// Edge-detection branch: preload 3x3 around near-edge position
		// plus 3 movement-ahead zones.
		void* zoneEntry = GetZoneEntry(zoneMgr, curGX, curGY);
		if (!zoneEntry)
			continue;

		float zoneCenterX = GetZoneCenterX(zoneEntry);
		float zoneCenterZ = GetZoneCenterZ(zoneEntry);

		float dx = curX - zoneCenterX;
		float dz = curZ - zoneCenterZ;
		float adx = (dx < 0.0f) ? -dx : dx;
		float adz = (dz < 0.0f) ? -dz : dz;

		if (adx > EDGE_THRESHOLD || adz > EDGE_THRESHOLD)
		{
			int adjX = curGX;
			int adjY = curGY;
			if (dx > EDGE_THRESHOLD) adjX = curGX + 1;
			else if (-dx > EDGE_THRESHOLD) adjX = curGX - 1;
			if (dz > EDGE_THRESHOLD) adjY = curGY + 1;
			else if (-dz > EDGE_THRESHOLD) adjY = curGY - 1;

			for (int j = 0; j < 9; ++j)
			{
				if (EnqueueCharacterZone(adjX + ORDER_DX[j], adjY + ORDER_DY[j]))
					edgePreloads++;
			}

			// 3 movement-ahead zones beyond the 3x3 grid
			EnqueueAheadZones(adjX, adjY, curGX, curGY, OWNER_CHARACTER);
		}
	}

	if (removals > 0 || edgePreloads > 0)
	{
		std::ostringstream ss;
		ss << "Active poll: " << numWatched << " watched"
		   << ", " << removals << " removed"
		   << ", " << edgePreloads << " edge zones";
		LogDebug(ss.str());
	}

	{
		static double lastWatchSummary = 0.0;
		if (now - lastWatchSummary > 5.0)
		{
			int withOrder = 0;
			for (int i = 0; i < numWatched; ++i)
				if (watchedChars[i].hasMoveOrder) withOrder++;
			std::ostringstream ss;
			ss << "Watched: " << numWatched << " total, "
			   << withOrder << " with move order";
			LogDebug(ss.str());
			lastWatchSummary = now;
		}
	}
}

void TieredCharacterPoll(void* zoneMgr, double now)
{
	// Active group poll: every ~1s (only when we have watched characters)
	if (numWatched > 0 && now - lastActivePoll > ACTIVE_POLL_INTERVAL)
	{
		PollActiveMovers(zoneMgr, now);
		lastActivePoll = now;
	}

	// Baseline scan: every ~5s (current-position preloading, skips watched chars)
	if (now - lastBaselineScan > BASELINE_SCAN_INTERVAL)
	{
		ScanCharacterZones(zoneMgr);
		lastBaselineScan = now;
	}
}


// =========================================================================
// Zone-lifecycle retention (main thread)
// =========================================================================
//
// The zones a watched mover stands in and steps into next: its current zone
// and one step along the sign of (destination - current), the direction
// PollActiveMovers preloads. Read from the stored fields only: the character
// pointer is validated against the player list by PollActiveMovers alone, so
// it is never dereferenced here. A baseline entry (dest == current) yields
// just its current zone.
int CollectMoverRetainZones(int* gx, int* gy, int cap)
{
	int n = 0;
	for (int i = 0; i < numWatched && n < cap; ++i)
	{
		if (!watchedChars[i].character)
			continue;
		int cx = watchedChars[i].currentZoneX;
		int cy = watchedChars[i].currentZoneY;
		if (cx < 0 || cx > ZONE_GRID_MAX || cy < 0 || cy > ZONE_GRID_MAX)
			continue;
		gx[n] = cx;
		gy[n] = cy;
		n++;

		int dirX = 0, dirY = 0;
		if (watchedChars[i].destZoneX > cx) dirX = 1;
		else if (watchedChars[i].destZoneX >= 0 && watchedChars[i].destZoneX < cx) dirX = -1;
		if (watchedChars[i].destZoneY > cy) dirY = 1;
		else if (watchedChars[i].destZoneY >= 0 && watchedChars[i].destZoneY < cy) dirY = -1;
		if ((dirX != 0 || dirY != 0) && n < cap)
		{
			gx[n] = cx + dirX;
			gy[n] = cy + dirY;
			n++;
		}
	}
	return n;
}

