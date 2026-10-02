#ifndef KEO_DIAG_MODULE_BASES_H
#define KEO_DIAG_MODULE_BASES_H

#include <cstddef>

// One loaded module, captured once at startPlugin. `name` is a truncated,
// always-NUL-terminated copy of the module's file name (no path); `size` is
// 0 when SizeOfImage could not be read, which ResolveModuleForAddress treats
// as "never matches" rather than guessing an extent.
struct ModuleBaseEntry
{
	char name[24];
	unsigned __int64 base;
	unsigned __int64 size;
};

// Generous headroom over an ordinary Kenshi process's DLL count; a fixed
// bound so the crash path never walks a loader list of unknown length.
const int kMaxModuleBases = 48;

// Appends "Module bases: name=0xBASE+0xSIZE ..." for as many of `count`
// entries as fit in `cap` bytes (NUL included), stopping before any entry
// would be written half-truncated. Returns the number of bytes written
// (excluding the NUL). *outTruncated (if non-NULL) is how many entries were
// left out, 0 if all of them fit -- a session log can then tell "these are
// all the modules" from "the list ran off the edge of the buffer".
size_t FormatModuleBases(char* out, size_t cap, const ModuleBaseEntry* mods, int count,
                          int* outTruncated);

// Finds the module whose [base, base+size) contains `addr`. Returns its
// index into `mods`, or -1 if none matches -- including every entry whose
// size is 0. On a match, *outOffset (if non-NULL) is addr - base, the
// "module+0xOFFSET" figure a crash record's `rva=` cannot give once `addr`
// falls outside the game executable.
int ResolveModuleForAddress(const ModuleBaseEntry* mods, int count,
                             unsigned __int64 addr, unsigned __int64* outOffset);

// Writes into `out` the module attribution for `addr`, as one of four
// distinct shapes so "no module contains it" is never confused with "we
// never got a module list to check against", and a module that loaded after
// the startup snapshot still gets an address, if not a name:
//   "NAME+0xOFF"    -- addr falls inside mods[i]
//   "image@0xB+0xO" -- not in mods, but imageBase != 0 (the caller's own,
//                      loader-lock-free check found addr inside *some*
//                      loaded image, just not one the snapshot named)
//   "none"          -- the list is real (haveList) but neither of the above
//   "?"             -- haveList is false and imageBase == 0: nothing here
//                      can place the address at all
// Same allocation-free, CRT-stream-free style as FormatModuleBases, so it is
// safe to call from the crash path. Returns the number of bytes written
// (excluding the NUL).
size_t FormatAddrMod(char* out, size_t cap, const ModuleBaseEntry* mods, int count,
                      bool haveList, unsigned __int64 addr, unsigned __int64 imageBase);

// Fills `out` (capacity `cap` entries) from `all` (`allCount` entries, the raw
// enumeration order, which may exceed `cap`), guaranteeing that every entry
// whose base matches one of `priorityBases` survives even when the process
// has more modules than `cap` can hold. Ordinary entries fill whatever room
// is left, in their original order. Priority entries not found in `all` are
// simply absent -- this never invents a base it wasn't given. Returns the
// number of entries written to `out`.
//
// Kept apart from the snapshot: the crash record's own module list is only as
// good as this choice of which entries to keep, and that choice is worth
// testing without a live process. cap and priorityCount are both expected to
// be small fixed numbers, so both loops below are bounded by them.
int SelectModuleBases(const ModuleBaseEntry* all, int allCount,
                       const unsigned __int64* priorityBases, int priorityCount,
                       ModuleBaseEntry* out, int cap);

// The snapshot startPlugin took, so a caller outside plugin/crash_record.cpp can resolve an
// address against it with ResolveModuleForAddress rather than building a
// second module table. It is a startup snapshot: a module loaded later is not
// in it, and an address inside one resolves to -1. Implemented in plugin/crash_record.cpp,
// which owns the snapshot.
const ModuleBaseEntry* CapturedModuleBases(int* outCount);

// The raw, uncurated startup enumeration (ahead of SelectModuleBases's cap),
// for resolving one address against every module present at startup, not
// just the ones the crash record's own table kept room for. A module loaded
// later is not in it (FormatAddrMod's imageBase parameter covers that case
// separately). *outValid is false only if the startup enumeration itself
// failed (see SnapshotModuleBases), which FormatAddrMod's "?" case reports
// as distinct from a real empty or non-matching list. Implemented in
// plugin/crash_record.cpp, which owns the snapshot.
const ModuleBaseEntry* FullModuleBases(int* outCount, bool* outValid);

#endif // KEO_DIAG_MODULE_BASES_H
