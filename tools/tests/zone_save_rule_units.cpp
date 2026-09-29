// The unload save rule is pure; every row of its table.
#include "zone/retention/zone_save_rule.h"
#include "check.h"

int main()
{
	Check(ZlSaveRuleDecide(false, false, 1, 0, true) == ZL_SAVE_DISCARD, "a zone never accessible is discarded, whatever its flags");
	Check(ZlSaveRuleDecide(true, false, 0, 1, false) == ZL_SAVE_KEEP, "an accessible zone whose flags could not be read is kept");
	Check(ZlSaveRuleDecide(true, true, 1, 1, false) == ZL_SAVE_KEEP, "an accessible first-time zone is kept");
	Check(ZlSaveRuleDecide(true, true, 0, 0, false) == ZL_SAVE_KEEP, "an accessible zone never finalized is kept");
	Check(ZlSaveRuleDecide(true, true, 0, 1, true) == ZL_SAVE_KEEP, "an accessible zone finalized twice is kept");
	Check(ZlSaveRuleDecide(true, true, 0, 1, false) == ZL_SAVE_SAVE, "an accessible, finalized, not first-time zone is saved");
	return CheckExit("zone_save_rule_units");
}
