// operator_policy.h - When a machine operator holds its load instead of delivering: pure. The
// evaluator's detour gathers the facts in this order and stops at the first that fails. No Windows,
// KenshiLib or game header; any thread.
#ifndef KEO_INVENTORY_OPERATOR_POLICY_H
#define KEO_INVENTORY_OPERATOR_POLICY_H

namespace keo_inventory {

// StorageBuilding's output kind for a resource (ITEM); gear benches have another value.
const int OPERATOR_RESOURCE_OUTPUT = 4;

enum OperatorReason
{
	OR_HOLD = 0,          // every guard holds: answer "not yet"
	OR_VANILLA_FALSE,     // vanilla already said "not wanted gone"
	OR_NOT_PLAYER,
	OR_NO_MACHINE,
	OR_NOT_OWNED,
	OR_NOT_RESOURCE,
	OR_UNPOWERED,
	OR_INPUTS_INVALID,
	OR_HUNGRY,
	OR_NO_PRODUCT,
	OR_NO_ROOM,
	OR_COUNT
};

struct OperatorFacts
{
	bool vanillaTrue, isPlayer, haveMachine, ownsMachine, isResource, powered, inputsValid, hungry,
	     haveProduct, hasRoom;
};

// The first failing guard in the enum's order, or OR_HOLD. A fact after the first failure is
// never read, so the detour may leave it unset.
OperatorReason OperatorReasonOf(const OperatorFacts& f);
// The detour's answer: false only on a hold; vanilla's otherwise.
bool OperatorAnswer(bool vanilla, OperatorReason r);
// The design's conjunction (machine and product presence folded into ownsMachine and hasRoom).
bool HoldDelivery(bool vanillaTrue, bool isPlayer, bool ownsMachine, bool isResource, bool hasRoom,
                  bool powered, bool inputsValid, bool hungry);
// The jobs heartbeat's field names, one per reason.
const char* OperatorReasonName(OperatorReason r);
// The haul histogram's bucket count: HaulBucket answers 0..HAUL_BUCKETS - 1.
const int HAUL_BUCKETS = 17;
// A haul size's histogram bucket: 0..15, and 16 (HAUL_BUCKETS - 1) for anything larger.
int HaulBucket(long long amount);

} // namespace keo_inventory

#endif
