#ifndef KENSHI_ZONE_OPT_NM_MISSPAR_H
#define KENSHI_ZONE_OPT_NM_MISSPAR_H

#include "base/config.h"
#include "navmesh/cache/nm_cache_types.h"
#include <sstream>

// Who holds processJobCS: set by the holder right after it acquires, cleared
// when it releases. A waiter samples it when its wait starts.
enum { MP_HOLD_NONE = 0, MP_HOLD_WMISS, MP_HOLD_BGMISS, MP_HOLD_T234, MP_HOLD_OTHER, MP_HOLD_KINDS };

// QPC stamps of one generated MISS, taken on the thread that ran it.
struct MissParJob
{
	LONGLONG waitStart, locked, jobAltStart, jobAltEnd, fixEnd, released, bcStart, bcEnd;
	int      holderAtWait;
	bool     worker;
};

void MissParHolderSet(int kind);
int  MissParHolderGet();
void MissParGenBegin();
void MissParGenEnd();
void MissParRecordJob(const MissParJob& j);
void MissParNoteHash(const NavMeshCacheEntry& e, bool worker);
void MissParAppendStats(std::ostringstream& ss);

// True once all five Havok keycode flags read non-zero (lock-free, any thread).
bool MissParKeycodesReady();
// Runs the game's own validator on any flag still reading 0. The caller must
// hold processJobCS; see nm_misspar.cpp for why that serializes it against
// every reader.
void MissParKeycodeWarmup();

#include "navmesh/scheduling/nm_inflight.h"

// Same type as game.h's; repeated so this header stays free of game headers.
typedef void (*nmResultPopulate_t)(void* navData, void* localData, void* result, int param);

// Once, on the main thread, after the config is loaded and before any dispatch.
void MissParInit();
// This thread is inside a MISS's processJobAlt: clone true when it runs on an
// NMG clone (its work buffer is the clone's own), false when it runs on the
// real generator with a fresh work buffer swapped into realNMG+256. realNMG is
// the real generator either way, so populate can compare the two.
void MissParArm(bool clone, void* realNMG);
void MissParDisarm();
// The populate hook's body: runs orig either under processJobCS as called, or
// with processJobCS released and a generation slot held. Returns with this
// thread's processJobCS hold exactly as on entry, on every path.
void MissParPopulate(nmResultPopulate_t orig, void* wb, void* local, void* mesh, int param);
// After the stop flag is set: wakes threads queued for a generation slot.
void MissParShutdownWake();
// True on the thread whose last populate gave processJobCS up for good because
// the stop was seen while re-acquiring it: processJobAlt returned without the
// lock, under the retire's cleanup handshake (NavMeshReenterAfterGenerate).
bool MissParLockLost();
// A type-1 splice skipped because the job's zone was unloaded.
void MissParNotePartialSkip();
// The save-load reset: from Begin until End no populate releases processJobCS.
// Begin then waits, up to timeoutMs, for the generations already released to
// be back under the lock (or to have given it up at the stop); false means
// some are still running when the bound expired.
bool MissParDrainBegin(DWORD timeoutMs, DWORD* waitedMs);
void MissParDrainEnd();

// Arms for the lifetime of the scope, so an unwind cannot leave the thread armed.
struct MissParArmScope
{
	MissParArmScope(bool clone, void* realNMG) { MissParArm(clone, realNMG); }
	~MissParArmScope() { MissParDisarm(); }
private:
	MissParArmScope(const MissParArmScope&);
	MissParArmScope& operator=(const MissParArmScope&);
};

// Owns one in-flight key for the scope. Register waits, holding no lock, while
// another thread generates the same key. Release (idempotent) lets the waiters
// go once the result is in L1 or the job has taken a late HIT; the destructor
// releases on every other exit.
class InflightScope
{
public:
	InflightScope() : slot(-1) {}
	~InflightScope() { Release(); }
	void Register(const NavMeshCacheKey& key);
	void Release() { if (slot >= 0) { InflightRelease(slot); slot = -1; } }
private:
	int slot;
	InflightScope(const InflightScope&);
	InflightScope& operator=(const InflightScope&);
};

#endif
