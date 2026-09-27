// cpp_exception.h — what a C++ throw was, captured where it is still knowable.
//
// A C++ throw reaches a vectored handler as code 0xE06D7363 with the throw's
// type information still intact in its exception record; by the time anything
// downstream runs, that record may be gone. The note below is therefore taken
// first-chance, on every throw, and is deliberately cheap: counters, an
// address and a name, no syscall and no file.
//
// Most throws here are caught and harmless, so the note alone never means a
// crash. Its value is on a record written for some other reason, and on the
// small number of throws that do get written -- an allocation failure that
// kills the process throws exactly once and is otherwise invisible.

#ifndef KENSHI_ZONE_OPT_DIAG_CPP_EXCEPTION_H
#define KENSHI_ZONE_OPT_DIAG_CPP_EXCEPTION_H

#include <stddef.h>

// `exceptionRecord` is an EXCEPTION_RECORD*; typed as void* so callers that
// have no Windows headers can still use this file. Ignores anything that is
// not a throw with usable type information, and never faults on one that is
// malformed.
void CppExceptionNote(const void* exceptionRecord);

long CppExceptionCount();

// Last thrown type name, "" when none was captured. Returns the length.
size_t CppExceptionLastType(char* out, size_t cap);

unsigned __int64 CppExceptionLastAddress();

// Claims the next on-disk record slot for a C++ throw and returns its number,
// or 0 when the admission rule in fatal_class.h refuses this one. Counted
// apart from the access-violation records, so a caught throw can never spend
// the budget those records live on.
long CppExceptionClaimRecordSlot(double nowSec);

// "cppEx=3(last=Ogre::RenderingAPIException @0x7FFE44DD0000)", or "cppEx=0".
// Written into a caller buffer with no allocation, so a crash record can use
// the same text the periodic line does.
size_t CppExceptionToken(char* out, size_t cap);

#endif // KENSHI_ZONE_OPT_DIAG_CPP_EXCEPTION_H
