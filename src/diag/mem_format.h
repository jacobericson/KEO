// mem_format.h — byte figures and their text form.
//
// Split from the sampler on purpose: nothing here touches Windows, so the
// same code the crash path prints with is exercised by the host tests. The
// formatters build into a caller-supplied buffer with no CRT streams and no
// allocation, because one caller is a vectored exception handler.

#ifndef KEO_DIAG_MEM_FORMAT_H
#define KEO_DIAG_MEM_FORMAT_H

#include <stddef.h>

// One reading. The two `have` flags are separate because the process figures
// and the system figures come from different calls, and either can fail on
// its own; a field whose flag is clear prints as "-" rather than as zero.
struct MemFigures
{
	unsigned __int64 privateCommit;    // process commit charge
	unsigned __int64 peakPrivate;      // high-water mark of the above
	unsigned __int64 workingSet;       // resident bytes
	unsigned __int64 pagefileFree;     // system commit still available
	unsigned __int64 pagefileTotal;    // system commit limit
	unsigned long    memLoadPct;       // system physical load, 0-100
	int              haveProcess;
	int              haveSystem;
};

void MemFiguresClear(MemFigures* f);

// Rounded to the nearest MB. Diagnostics are read at GB scale; the rounding
// keeps a 10.9 GB figure from reading as 10 GB.
unsigned long MemBytesToMB(unsigned __int64 bytes);

// "privMB=11318 peakPrivMB=11500 wsMB=9042 sysCommitFreeMB=1900
//  sysCommitTotalMB=73600 sysLoad=88%" -- the periodic line's form.
// Returns the number of characters written (always NUL-terminated).
size_t MemFormatLong(char* buf, size_t cap, const MemFigures& f);

// "priv11318MB/free1900MB/load88%" -- the compact form for lines that already
// carry a lot of tokens. Same return contract.
size_t MemFormatShort(char* buf, size_t cap, const MemFigures& f);

#endif // KEO_DIAG_MEM_FORMAT_H
