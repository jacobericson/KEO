// zone_save_rule.cpp - The mod unload's save rule. No game or platform types.
#include "zone/retention/zone_save_rule.h"

ZlSaveVerdict ZlSaveRuleDecide(bool accessible, bool flagsRead, int firstTime, int loaded, bool doubleFinalized)
{
	if (!accessible)
		return ZL_SAVE_DISCARD;
	if (!flagsRead || firstTime != 0 || loaded == 0 || doubleFinalized)
		return ZL_SAVE_KEEP;
	return ZL_SAVE_SAVE;
}
