#include "zone/readiness/zone_readiness_contract.h"
#include "zone/zone_ledger_core.h"

ZoneReadinessAnswer ZoneReadinessContractDecide(int cellClass, bool globalGameOwnedBypass)
{
	if (globalGameOwnedBypass)
		return ZONE_READY_ANSWER_ORIGINAL;

	switch (cellClass)
	{
	case ZONE_CLASS_PRIVATE:
	case ZONE_CLASS_ADOPTED:
		return ZONE_READY_ANSWER_ORIGINAL;
	default:   // ZONE_CLASS_NONE, or a value the enum does not have: the game's own to load
		return ZONE_READY_ANSWER_TODAY_BYPASS;
	}
}

ZoneReadinessBucket ZoneReadinessContractBucket(int cellClass, bool globalGameOwnedBypass)
{
	if (globalGameOwnedBypass)
		return ZONE_READY_BUCKET_GLOBAL;

	switch (cellClass)
	{
	case ZONE_CLASS_PRIVATE: return ZONE_READY_BUCKET_PRIVATE;
	case ZONE_CLASS_ADOPTED: return ZONE_READY_BUCKET_ADOPTED;
	default:                 return ZONE_READY_BUCKET_NONE;
	}
}
