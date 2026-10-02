// exit_capture.h -- DEV-only instrument for the case where the game vanishes
// to the desktop with no dialog, no crash_dump.txt and no WER dump. RE_Kenshi
// sets SEM_NOGPFAULTERRORBOX, so the VS2010 CRT's invalid-parameter and
// /GS-failure paths can call UnhandledExceptionFilter directly and terminate
// the process without ever raising a dispatched SEH exception -- nothing the
// mod's existing VEH (plugin/crash_record.cpp) can see, because it only ever runs on a real
// exception dispatch.
//
// This installs two DEV-only detours on exported OS functions -- never on a
// game-exe site, so it never touches the build gate's "gate=" token or the
// "N/N hooks installed" count -- and arms them only once the game has
// installed its own NavMesh::stop hook and not yet begun a normal shutdown.
// Compiled into every variant; a true no-op outside KEO_DEBUG.

#ifndef KEO_DIAG_EXIT_CAPTURE_H
#define KEO_DIAG_EXIT_CAPTURE_H

#include <string>

// Call once from startPlugin, after AddVectoredExceptionHandler/
// SetUnhandledExceptionFilter and after LoadConfig, with the same dllDir
// InitLogFile already resolved. Logs one line naming which pieces of
// plumbing came up (diag/exit_capture_policy.h: ExitCaptureInstallLogLine).
// That line does not mean the instrument can fire yet -- see
// ExitCaptureNoteStopHookOutcome below. Kernel32 and ntdll are already
// mapped by this point in startPlugin, so unlike PurecallRecord's
// PhysXCore64.dll wait, there is no deferred retry here.
void InstallExitCapture(const std::string& dllDir);

// Call from the NavMesh::stop hook's own install site (nm_lazy_hooks.cpp),
// exactly once, with whether that install itself succeeded. Logs whether the
// instrument can really arm this session (it needs the stop hook's own
// install to tell a normal quit apart from a silent one) or stays off.
// A true no-op outside KEO_DEBUG.
void ExitCaptureNoteStopHookOutcome(bool installed);

#endif // KEO_DIAG_EXIT_CAPTURE_H
