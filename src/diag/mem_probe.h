// mem_probe.h — process and system memory figures.
//
// Two readings answer two different questions. The process figures say
// whether this process is growing; the system commit figures say whether the
// machine had any commit left to give, which is the difference between "we
// leaked" and "another process filled the page file". Both belong on a crash
// record, because an allocation failure cannot be attributed without them.
//
// Every read is a call through a pointer resolved once at startup into two
// kernel32 entry points that query the kernel and copy into a caller buffer.
// There is no allocation, no CRT stream, no lock and no loader work, so the
// live read is usable from a vectored exception handler.

#ifndef KEO_DIAG_MEM_PROBE_H
#define KEO_DIAG_MEM_PROBE_H

#include "diag/mem_format.h"

// Resolves the two entry points and takes the first sample. Call once from
// startPlugin. Safe to call twice.
void MemProbeInit();

// Live read. False leaves *out cleared, which its `have` flags then report.
bool MemProbeRead(MemFigures* out);

// Live read published into the process-wide last sample, stamped with `now`.
void MemProbeSampleNow(double now);

// The last published sample and its age in seconds. False when none exists.
bool MemProbeLastSample(MemFigures* out, double* ageSec, double now);

// The unconditional periodic line. No INI flag, no build step and no caller
// state gates it: a memory figure that stopped printing under the conditions
// it exists to describe would be worth nothing. Throttles itself to one line
// per 30 s, so the per-frame cost is the clock comparison already made by the
// caller plus one double subtraction.
void LogMemoryStats(double now);

// The compact form of the last sample, for a line that already carries many
// tokens. Uses the published sample, never a fresh syscall.
size_t MemProbeShortLast(char* buf, size_t cap);

// The same, followed by the sample's age (" memSrc=last+12s", or
// " memSrc=none"), for a line whose own cadence is not the sampler's.
size_t MemProbeShortLastAged(char* buf, size_t cap, double now);

#endif // KEO_DIAG_MEM_PROBE_H
