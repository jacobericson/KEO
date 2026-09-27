#pragma once
// The readiness contract for hook_isContentPending: what the hook answers
// once deferral is on and the original has already said "not ready". Only
// consulted at ZONEHAND_STEP >= 2 (readiness_hook.cpp); below that the hook keeps its
// pre-existing islandReadinessRule/today's-bypass behaviour untouched.
//
// Ruling: adopted and private cells hear the original (no override: the
// hook's answer here is false, since the original already said so); every
// other cell -- unclassified, because the mod never reached it -- is the
// game's own to load and keeps today's sections==0 bypass. There is no
// third answer: a distinct "game-owned" class is buildable (a writer could
// observe every native activation, not only mod-held cells), but it would
// be behaviourally inert, since it would answer identically to unclassified
// -- today's bypass -- so islandReadinessRule is not consulted from this
// contract at all, for either. The global game-owned bypass (ZM+8,
// justLoadedAGame) is not per-class: while it is set, every cell hears the
// original, checked before the per-cell class is even asked.
//
// Pure decision logic: no game or KenshiLib headers, no allocation, no side
// effects. Given the same two inputs it returns the same answer regardless
// of which thread calls it, which is what lets hook_isContentPending keep
// its "same return value on every thread" rule. Host-tested under
// tools/tests/zone_readiness_contract_units.cpp. The caller supplies
// cellClass from the ledger (zone_readiness_ledger_bridge.h, a separate
// non-host-tested unit, the same split zone_cycle_math.h/zone_cycle_stats.h
// already use) and globalGameOwnedBypass from a direct ZM+8 read -- see
// readiness_hook.cpp's call site for why that flag is read fresh rather than
// mirrored.

enum ZoneReadinessAnswer
{
	ZONE_READY_ANSWER_ORIGINAL,     // hear the original: the hook returns false, no override
	ZONE_READY_ANSWER_TODAY_BYPASS  // apply today's sections==0 bypass; islandReadinessRule is not consulted
};

// cellClass carries a ZoneReadinessClass value (zone_ledger_core.h): 0 =
// none/unknown, 1 = private, 2 = adopted, passed as a plain int so this
// declaration needs no ledger include. Any value outside that range is
// treated the same as none (today's bypass), never as a crash or an
// assumption about a class the enum does not have.
ZoneReadinessAnswer ZoneReadinessContractDecide(int cellClass, bool globalGameOwnedBypass);

// Which class the contract saw for a cell it was consulted about -- the
// classification itself, not the decision taken from it. The caller reads
// this for every call it is asked about, including calls whose answer was
// already settled before the decision point, so a bucket reading zero means
// no cell of that class was consulted, never that the counting path was
// skipped. GLOBAL masks the per-cell class: while the bypass is set, a
// private or adopted cell reports GLOBAL, so a reading taken during a load
// says nothing about whether the other two arms are reachable.
enum ZoneReadinessBucket
{
	ZONE_READY_BUCKET_NONE = 0,   // unclassified, or a value the enum does not have
	ZONE_READY_BUCKET_PRIVATE,
	ZONE_READY_BUCKET_ADOPTED,
	ZONE_READY_BUCKET_GLOBAL      // the bypass is set: the per-cell class is not what would decide
};

ZoneReadinessBucket ZoneReadinessContractBucket(int cellClass, bool globalGameOwnedBypass);
