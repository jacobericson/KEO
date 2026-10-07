// backpack_window.h - The backpack-first tick box in a worn backpack's window.
#ifndef KEO_INVENTORY_BACKPACK_WINDOW_H
#define KEO_INVENTORY_BACKPACK_WINDOW_H

namespace keo_inventory {

// Main thread, a startup install step after InstallBackpackSidecar: checks the three layout
// tripwires, then installs the setupSections post-hook. A refusal leaves backpack windows vanilla.
void InstallBackpackWindow(int* installed, int* total);

// Interlocked reads, for the Backpack: heartbeat: boxes added, windows skipped by the filter,
// windows without an arrange button, clicks applied, clicks refused.
void BackpackWindowCounters(long out[5]);

} // namespace keo_inventory

#endif
