#include "movement/k7_swap_policy.h"

const double K7_SIG_WINDOW = 3.0;

namespace k7_swap_policy_detail {

// KenshiLib Enums.h TaskType: EQUIP_WEAPON=6, MELEE_ATTACK=4 and its
// neighbours, ranged and thrown variants, SELF_PRESERVATION=32, RUN_AWAY=35,
// STAND_STILL=61/62, a forced stumble=147, and the rest of the combat and
// combat-recovery family.
const int kCombatTasks[] = {
	4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 16, 17, 21, 32, 34, 35,
	61, 62, 64, 143, 147, 178, 181
};
const int kCombatTaskCount = sizeof(kCombatTasks) / sizeof(kCombatTasks[0]);

} // namespace
using namespace k7_swap_policy_detail;

bool K7IsCombatTask(int curType)
{
	for (int i = 0; i < kCombatTaskCount; ++i)
		if (kCombatTasks[i] == curType) return true;
	return false;
}

double K7SigOnsetStep(double onset, bool sigThisFrame, bool task29Poll, double now)
{
	if (task29Poll) return 0.0;
	if (onset <= 0.0 && sigThisFrame) return now;
	return onset;
}

bool K7DestReadyAllows(int cls, double waitedSec, double maxWaitSec)
{
	if (cls == 2 /* ZR_BUILDINGS_PENDING */) return true;
	return waitedSec >= maxWaitSec;
}

K7SwapVerdict K7ClassifySwap(double last29, double sig, double deletedSince,
                             double swapSeen, double now, int curType,
                             double margin, double holdMax)
{
	bool diedFirst = (sig > 0.0 && sig > last29 && (swapSeen - sig) >= margin)
	              || (deletedSince > 0.0 && sig > 0.0 && sig >= deletedSince - K7_SIG_WINDOW);
	if (!diedFirst)
		return K7_SWAP_DROP;
	if (!K7IsCombatTask(curType))
		return K7_SWAP_DROP_NONCOMBAT;
	double since = (deletedSince > 0.0) ? deletedSince : sig;
	if (now - since > holdMax)
		return K7_SWAP_EXPIRED;
	return K7_SWAP_HOLD;
}
