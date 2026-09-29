// zone_save_rule.h - The mod unload's save rule as a pure decision. No game or platform types.
#ifndef KENSHI_ZONE_OPT_ZONE_SAVE_RULE_H
#define KENSHI_ZONE_OPT_ZONE_SAVE_RULE_H

enum ZlSaveVerdict { ZL_SAVE_DISCARD = 0, ZL_SAVE_SAVE, ZL_SAVE_KEEP };

// A zone never accessible holds nothing the player did: discard. An
// accessible zone is saved only when its content flags were read, it is not
// first-time, it was finalized and never finalized twice; any other
// accessible zone is kept.
ZlSaveVerdict ZlSaveRuleDecide(bool accessible, bool flagsRead, int firstTime, int loaded, bool doubleFinalized);

#endif
