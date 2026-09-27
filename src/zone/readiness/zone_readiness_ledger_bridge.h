#pragma once
// Supplies zone_readiness_contract.h's per-cell input from the ledger
// (zone_prep_ledger.h's shared g_zonePrepLedger instance). Gated
// ZONEHAND_STEP >= 2; below that step every cell classifies as unknown, so
// hook_isContentPending's existing behaviour (islandReadinessRule or
// today's bypass, unchanged) is exactly what runs. Not a header-clean unit
// (needs config.h and, at step 2, the ledger): declared separately from
// zone_readiness_contract.h so that pure decision stays host-testable
// without this file.
//
// The global game-owned bypass (ZM+8, justLoadedAGame) is read directly at
// the decision point instead of through this ledger's mirrored copy: a
// synchronous call (SaveManager::loadGame) sets the live flag well before
// any frame boundary the ledger could observe it at, so a once-per-frame
// mirror can lag behind the live flag for the whole span of that call --
// exactly the window this bypass exists to cover. See readiness_hook.cpp's use of
// KLIB_MEMBER(..., ZoneManager_justLoadedAGame, OFF_ZM_LOADING) on
// g_cachedZoneMgr, alongside the other live ZoneManager fields that
// hook already reads on whatever thread calls it.

#include "base/config.h"

// Bounds-checks (gx, gy) itself before touching the ledger: a coordinate
// outside 0..63 answers unknown (ZONE_CLASS_NONE), the same as a cell the
// ledger has never seen, rather than reading through ZoneCellIndex's clamp
// into some other cell's class.
int ZoneReadinessBridgeClassOf(int gx, int gy);
