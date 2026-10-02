// fatal_class.h — naming the kind of a fatal event, and the thrown C++ type.
//
// Pure: no Windows types, no memory walking, so the host tests cover the two
// pieces that decide what a crash record says it is. The reads that produce
// the raw type descriptor name live next door in cpp_exception.cpp.

#ifndef KEO_DIAG_FATAL_CLASS_H
#define KEO_DIAG_FATAL_CLASS_H

#include <stddef.h>

// Record kinds. Each prints as its own token so no two causes of death ever
// merge in the log: an access violation and a C++ exception carrying an
// allocation failure are different events with different evidence.
enum FatalKind
{
	FATAL_KIND_AV = 0,        // access violation
	FATAL_KIND_DIV0,          // integer divide by zero
	FATAL_KIND_STACK,         // stack overflow
	FATAL_KIND_CPPEX,         // C++ throw (0xE06D7363), seen first-chance
	FATAL_KIND_FASTFAIL,      // CRT/security abort surfaced as an exception
	FATAL_KIND_BREAK,         // breakpoint / single step
	FATAL_KIND_OTHER
};

// The token written into the record. Never NULL.
const char* FatalKindToken(int kind);

// Maps a Win32 exception code to the kind above.
int FatalKindForCode(unsigned long code);

// The `access=` / `rw=` pair for a fault record, written into `out` as one
// NUL-terminated run ("access=0x00000007408687BC rw=0/read"). Returns the
// length, excluding the NUL.
//
// `access` is ExceptionInformation[1], the address the faulting instruction
// tried to touch, and `rw` is ExceptionInformation[0]. The exception address
// alone cannot tell a NULL dereference from a wild pointer -- that is the
// whole reason this pair exists -- so both always print, and both print `-`
// rather than 0 when the code does not carry them (an integer divide by zero
// leaves ExceptionInformation meaningless) or the record declares fewer than
// two parameters. `access=0x0` is a real reading; `access=-` is not one.
//
// `rw` keeps its raw number next to the token so an access type outside the
// documented 0/1/8 survives into the record instead of being folded into one
// of three names.
size_t FatalFormatAccess(unsigned long code, unsigned long numberParameters,
                         unsigned __int64 info0, unsigned __int64 info1,
                         char* out, size_t cap);

// Turns an MSVC type descriptor name into something readable:
// ".?AVRenderingAPIException@Ogre@@" -> "Ogre::RenderingAPIException".
// Anything that does not parse is copied through verbatim, because a raw
// descriptor still names the type. Always NUL-terminates; returns the length.
size_t FatalDemangleTypeName(const char* raw, char* out, size_t cap);

// Whether a first-chance C++ throw earns an on-disk record.
//
// Two failure modes have to be avoided at once. Recording every throw would
// turn a chatty but healthy session into a write storm; recording only the
// first few would capture a session's opening throws and miss the one that
// killed it minutes later, which is the case this exists for. So the opening
// few are taken unthrottled, everything after them is spaced out, and a hard
// cap bounds the total. `lastWriteSec` is negative when nothing has been
// written yet.
bool CppRecordAdmit(long recordsWritten, double nowSec, double lastWriteSec);

// The constants behind the rule above, exposed so a test names them once.
extern const long   CPP_RECORD_BURST;
extern const long   CPP_RECORD_CAP;
extern const double CPP_RECORD_SPACING_SEC;

#endif // KEO_DIAG_FATAL_CLASS_H
