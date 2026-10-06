// nm_force_rebuild_policy.h - The rebuild-navmesh key's pure rules: a force mark's word and its
// transitions, the cache bypass, the cells a press covers, the queue's front and the panel hold.
// No Windows or game header.
#ifndef KEO_NM_FORCE_REBUILD_POLICY_H
#define KEO_NM_FORCE_REBUILD_POLICY_H

const int    NM_REBUILD_GRID          = 64;     // cells per axis; a cell's index is gx * 64 + gy
const int    NM_REBUILD_MAX_CELLS     = 4;      // the camera's cell and at most three neighbours
const float  NM_REBUILD_EDGE_FRACTION = 0.25f;  // within this share of a side, the cell across it is rebuilt too
const double NM_REBUILD_MARK_TTL_SEC  = 60.0;   // a mark older than this forces nothing
const double NM_REBUILD_HOLD_CAP_SEC  = 60.0;   // the panel is never held longer after a press
const long   NM_MARK_SEQ_MAX          = 0x1FFFFFFF;

enum NmMarkState { NM_MARK_NONE = 0, NM_MARK_MARKED, NM_MARK_CLAIMED, NM_MARK_DONE };

// A mark's word: the press's sequence above two state bits. Sequences run
// 1..NM_MARK_SEQ_MAX, so a zero word is NONE with no press, and a job that
// finishes compares the whole word: it can only end the mark it claimed.
inline long NmMarkWord(long seq, int state) { return (long)(((unsigned long)seq << 2) | (unsigned long)(state & 3)); }
inline int  NmMarkStateOf(long word)         { return (int)(word & 3); }
inline long NmMarkSeqOf(long word)           { return (long)((unsigned long)word >> 2); }
long NmMarkNextSeq(long seq);

// A claim: CONSUME a MARKED mark with a type-0 job within the TTL; EXPIRED past
// it (the main thread clears it); NONE otherwise. A type-1 job's cache key is
// not the exterior tile's, so it never consumes one.
enum NmMarkClaim { NM_CLAIM_NONE = 0, NM_CLAIM_CONSUME, NM_CLAIM_EXPIRED };
NmMarkClaim NmMarkClaimDecide(long word, double ageSec, int jobType);

// A press on a cell: KEEP a mark a job is working on (CLAIMED within the TTL),
// so a second press waits for that job; MARK anything else afresh.
enum NmMarkPress { NM_PRESS_MARK = 0, NM_PRESS_KEEP };
NmMarkPress NmMarkPressDecide(long word, double ageSec);

// A press's cell has ended: its mark is DONE or NONE, or names another press.
bool NmMarkPressFinished(long pressWord, long nowWord);

// The queue's front: a type-0 job of a cell whose mark is MARKED within the TTL.
bool NmMarkWantsFront(long word, double ageSec, int jobType);

// A forced job reads neither L1 nor L2: the entry it would read is the one it replaces.
inline bool NmJobMayReadCache(bool keyOk, bool forced) { return keyOk && !forced; }

// The neighbours a press covers. The cell and its in-cell fraction are the
// engine's: floorf((size * 32 + p) / size). Nothing is selected unless that
// cell is (cellX, cellY) inside the grid. On each axis a fraction below the
// edge fraction selects the cell before, above 1 - it the cell after; both
// axes add the diagonal. Cells outside the grid are dropped and counted.
struct NmRebuildCell { int gx, gy; };
struct NmRebuildSelection
{
	bool          matched;
	float         fracX, fracZ;
	int           count;
	int           dropped;
	NmRebuildCell n[3];
};
void NmRebuildSelect(float worldX, float worldZ, float sizeX, float sizeZ, int cellX, int cellY,
                     NmRebuildSelection* out);

// Whether a cell can take an exterior job from the main thread with no
// navmesh lock: present, accessible, not being loaded by the mod, with content
// and terrain collision.
enum NmRebuildSkip { NM_SKIP_NONE = 0, NM_SKIP_NO_ZONE, NM_SKIP_PRIVATE, NM_SKIP_NOT_ACCESSIBLE,
                     NM_SKIP_NO_CONTENT, NM_SKIP_NO_TERRAIN };
NmRebuildSkip NmRebuildEligible(bool haveZone, bool accessible, bool beingLoaded, bool content, bool terrain);
const char*   NmRebuildSkipName(int skip);

// The key's call: any captured return address, as an offset from the image
// base, equal to keyRet. A detour another plugin put in front of this one
// moves the game's return address one frame down.
bool NmRebuildIsKeyCaller(const unsigned __int64* offsets, int n, unsigned __int64 keyRet);

// The hold: only an off-main dismissal, and only under the cap.
bool NmHoldSuppresses(bool active, bool onMainThread, double sincePressSec);
enum NmHoldVerdict { NM_HOLD_IDLE = 0, NM_HOLD_PENDING, NM_HOLD_DONE, NM_HOLD_CAPPED };
NmHoldVerdict NmHoldDecide(bool active, int unfinished, double sincePressSec);
// A held dismissal is issued again only as the game's own site would: the
// zone manager idle (loadingPhase 0) and no save load running.
bool NmHoldReplayNow(bool owed, bool haveZoneManager, int loadingPhase, bool saveLoading);

#endif
