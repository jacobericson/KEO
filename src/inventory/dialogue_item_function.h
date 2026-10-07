// dialogue_item_function.h - The dialogue item-function condition sees the worn backpack: the one
// hasItemFunction call in checkConditions goes through a near-page thunk to a wrapper.
#ifndef KEO_INVENTORY_DIALOGUE_ITEM_FUNCTION_H
#define KEO_INVENTORY_DIALOGUE_ITEM_FUNCTION_H

namespace keo_inventory {

// Main thread, at startPlugin before any world exists: verifies the call site and writes it while
// backpackDialogueFunction is on and the build gate passed. Logs one line either way.
void InstallDialogueItemFunctionPatch(bool gateOk);
// Any thread.
void DialogueItemFunctionCounters(long* calls, long* backpackHits);

} // namespace keo_inventory

#endif
