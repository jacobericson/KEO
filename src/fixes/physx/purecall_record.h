#ifndef KENSHI_ZONE_OPT_FIXES_PURECALL_RECORD_H
#define KENSHI_ZONE_OPT_FIXES_PURECALL_RECORD_H

// Installs a forensic recorder on PhysXCore64.dll's own pure-virtual-call
// handler slot (see purecall_layout.h for the verified addresses). A pure
// virtual call inside PhysX's statically linked CRT tail-jumps into its own
// abort(), which is a CRT exit, not an SEH exception: the mod's vectored
// handler (plugin/crash_record.cpp) can never see it, and the process dies -- an R6025
// popup, then RE_Kenshi's own teardown wedge -- with no crash_dump.txt. This
// does not change that death (see purecall_record.cpp for why). It only
// captures who was calling what, into purecall_dump.txt, in the instant
// before abort() runs.
//
// Call once from startPlugin, after LoadConfig. It verifies PhysXCore64's
// bytes itself before writing anything, so it is safe to call unconditionally
// once the build gate has already refused an unknown exe; a true no-op when
// `enabled` is false. PhysXCore64.dll is usually not loaded yet at this
// point (see PurecallRecordTick below); when it isn't, this logs a
// "deferred" line and leaves the retry to the tick.
void InstallPurecallRecorder(bool enabled);

// Call every main-thread hook_updateCameraZone frame, with the same
// ElapsedSec() clock the caller already computes for its own periodic ticks.
// A no-op unless a deferred install is still pending: throttled to at most
// one PhysXCore64.dll probe per second, and gives up (logging once) after a
// minute of the DLL never appearing. Main thread only -- it can call
// LogMsg and touch PhysXCore64 memory directly, unlike the handler itself.
void PurecallRecordTick(double now);

// Restores PhysXCore64's handler slot to whatever it held before install, but
// only if the slot still holds exactly the pointer we wrote there (compared
// as a raw encoded qword, never decoded) -- otherwise something else has
// taken the slot over since and is left alone. A no-op if the recorder was
// never armed. Call from DllMain's DLL_PROCESS_DETACH, the same place
// plugin_entry.cpp already unregisters the mod's vectored exception handler: if this
// DLL unloads while PhysX (and other threads) can still run, a stale pointer
// into our own freed code would turn a diagnosable abort into a crash into
// nothing. No logging, no allocation -- safe under the loader lock.
void UninstallPurecallRecorder();

#endif // KENSHI_ZONE_OPT_FIXES_PURECALL_RECORD_H
