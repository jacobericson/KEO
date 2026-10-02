#ifndef KEO_MISSPAR_MATH_H
#define KEO_MISSPAR_MATH_H

#include <string>

struct MissParSpan { const void* data; int bytes; };
// FNV-1a 64 over each span's byte count (4 bytes, little endian) then its bytes,
// so moving a boundary between spans changes the hash.
unsigned __int64 MissParFnv64(const MissParSpan* spans, int n);

struct MissParInterval { __int64 start, end; };
// Length of the union of the intervals (sorted in place by start); *sumOut is
// their plain sum. Union == sum means no two overlapped.
__int64 MissParUnion(MissParInterval* v, int n, __int64* sumOut);

// How many realGenerate calls may run at once. cfg > 0: cfg, at most cap.
// 0 (automatic): two, less on a machine without three cores to spare, never
// below one.
int MissParGenConcurrency(int cfg, int logicalCpus, int cap);

// The startup line stating what navmeshGenConcurrency resolved to, so a
// session log says the real cap instead of leaving it to be inferred from
// cpu count. splitOff notes when the cap governs nothing this session
// (navmeshMissSplit is off, so every generation still runs serially under
// processJobCS regardless of the cap).
std::string MissParGenConcurrencyMessage(int cfg, int cap, int logicalCpus, bool splitOff);

// What a populate is allowed to do with processJobCS.
enum { MP_ARM_SERIAL = 0, MP_ARM_CLONE, MP_ARM_SWAP };
enum { MP_REL_NONE = 0, MP_REL_CLONE, MP_REL_SWAP };

struct MissParReleaseInputs
{
	int         armKind;           // MP_ARM_*
	bool        splitEnabled;      // navmeshMissSplit
	bool        bgSplitEnabled;    // navmeshMissSplitBg
	bool        slotsReady;        // the generation semaphore exists
	bool        keycodesReady;
	bool        stopSeen;
	bool        holderIsBg;        // the lock this thread holds was taken for a background MISS
	int         pjDepth;           // this thread's processJobCS hold count
	int         swapOutstanding;   // threads between a swap arm and its disarm
	const void* wb;                // the work buffer populate was handed
	const void* installedWb;       // *(realNMG+256)
	const void* canonicalWb;       // the generator's own work buffer, 0 while unproven
};

// A clone generates on a work buffer that is not the one installed at
// realNMG+256, so nothing outside the job can reach it. A swap run generates on
// the fresh buffer it installed there, which only makes it private once the
// caller puts the canonical buffer back for the duration; that needs the
// canonical pointer, and it needs to be the only swap run in flight, because two
// of them would restore each other's buffer.
//
// A swap run also has to be the background thread's. Any other thread reaching
// this arm is a worker whose clone construction failed, and a worker must keep
// the re-acquire that lets it hand its job back at the stop, which the swap
// path's unconditional one does not.
int MissParClassifyRelease(const MissParReleaseInputs& in);

// Proving which work buffer is the generator's own. Observations are pointers
// read at a moment when no swap is installed; the same pointer seen needAgree
// times is taken as canonical, and a different one after that disables the swap
// release for the session rather than restoring a buffer that may be a fresh
// one.
struct MissParCanonState { const void* cand; int agree; bool disabled; };
void        MissParCanonInit(MissParCanonState& st);
void        MissParCanonObserve(MissParCanonState& st, const void* observed, int needAgree);
const void* MissParCanonConfirmed(const MissParCanonState& st, int needAgree);

#endif
