// log_bounded.h — the log's two lock-bounded primitives, with no game header

#ifndef KEO_LOG_BOUNDED_H
#define KEO_LOG_BOUNDED_H

#include <windows.h>

// Takes cs unless another thread keeps it for boundMs; true with it held.
bool EnterCriticalSectionBounded(CRITICAL_SECTION* cs, unsigned boundMs);

// Appends len bytes to path in one write, creating the file if needed, and
// flushes it to disk. No lock, no CRT stream.
bool AppendLineRaw(const char* path, const char* text, size_t len);

#endif
