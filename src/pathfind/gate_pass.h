#ifndef KEO_GATE_PASS_H
#define KEO_GATE_PASS_H

#include "base/config.h"
#include <string>

typedef bool (*gatesFindPath_t)(__int64 input, __int64 fromEntry, __int64 toEntry);
extern gatesFindPath_t orig_gatesFindPath;

// Path thread, inside Gates__updateCodes.
bool hook_gatesFindPath(__int64 input, __int64 fromEntry, __int64 toEntry);
// Path thread, from hook_gatesUpdateCodes around its original.
void GatePassBegin(void* gatesObj, LONGLONG entryQpc);
void GatePassEnd(LONGLONG exitQpc);
// Path thread, from PathPoolNoteSearch for a search inside a gate pass.
void GatePassNoteCause(int cause);
// Any thread, allocation-free: the loading screen was dismissed.
void GatePassNoteDismissal(LONGLONG qpc, LONG gen, bool onMain);
// Main thread: prints the GatePass: lines of passes that ended over 2 s ago.
void GatePassTickMain();
// Main thread: " gatePasses=<in bracket> preDismiss=<n> lastPassToDismiss=<ms|->".
std::string GatePassTransitionToken(LONG gen, LONGLONG endQpc);

#endif
