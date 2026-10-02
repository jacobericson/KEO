// coarse_graph.h - The route planner's coarse graph store: one directory entry per section, each
// holding an immutable base block and a per-world over block, published lock-free through an epoch
// word; a retire stack only the main thread frees; the interior uid table (main thread only); the
// live buffer pool and the per-slot live records the path thread posts into; and the cross-section
// arcs, resolved at read time. No KenshiLib or game header.
#ifndef KENSHI_ZONE_OPT_PLANNER_COARSE_GRAPH_H
#define KENSHI_ZONE_OPT_PLANNER_COARSE_GRAPH_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "planner/tile_graph_extract.h"

namespace planner {

const int CG_EXTERIOR_SLOTS    = 4096;   // gy * 64 + gx
const int CG_INTERIOR_SLOTS    = 4096;   // an insert-only open-addressed uid table; main thread only
const int CG_DIR_SLOTS         = CG_EXTERIOR_SLOTS + CG_INTERIOR_SLOTS;
const int CG_NODE_BITS         = 10;     // node key = (directory index << CG_NODE_BITS) | node
const int CG_NODE_ARCS_MAX     = 64;     // arcs a node reports to the search; equals COARSE_ARCS_MAX
const int CG_LIVE_BUFFERS      = 128;    // a 7x7 exterior window and its interiors
const int CG_LIVE_RECORDS      = 1024;   // one per collection slot: a packed face key's 10 slot bits
const int CG_LIVE_MAX_NODES    = 512;
const int CG_LIVE_MAX_ARCS     = 4096;
const int CG_LIVE_MAX_BORDERS  = 1024;
const int CG_INTERIOR_PROBES   = 64;
const float CG_PORTAL_SLACK    = 20.0f;  // world units: a far portal or box this near a border's portal

enum CgSource { CG_BASE = 0, CG_SAVE = 1, CG_LIVE = 2 };

struct CgNode   { float centre[3]; float boxMin[3]; float boxMax[3]; int faces; int water; int firstArc; int arcCount; int firstBorder; int borderCount; };
struct CgArc    { int to; float cost; };                          // intra, world units
struct CgBorder { int oppUid; int face; int oppFace; int from; float portal[3]; float a[3]; float b[3]; };

// An immutable section graph. Heap blocks come from _aligned_malloc(size, MEMORY_ALLOCATION_ALIGNMENT)
// with their arrays in the same allocation; a live buffer is a pool block with the same layout and is
// never published into the directory. borders are sorted by (oppUid, face); nodeBorders lists each
// node's border indices, node by node.
struct CgBlock
{
	SLIST_ENTRY link;          // the retire stack, the live free list and the builder's interior
	                           //   hand-off; first member, aligned
	int         uid;
	int         source;        // CgSource
	unsigned    storeGen;      // the store generation it was built under; 0 for base blocks
	unsigned    liveSeq;       // a live copy's acquire sequence; 0 for base and save blocks
	int         liveBuffer;    // pool index for a live buffer, -1 for a heap block
	int         collSlot;      // a live buffer's collection slot, -1 otherwise
	int         arcsTrunc;     // nodes whose intra arcs plus borders exceed CG_NODE_ARCS_MAX
	int         nodeCount, arcCount, borderCount;
	CgNode*     nodes;
	CgArc*      arcs;
	CgBorder*   borders;
	int*        nodeBorders;
};

// Lifecycle. Main thread.
bool     CgStoreCreate();          // the directory, the interior table, the live pool and records;
                                   //   false when an allocation fails
void     CgStoreDestroy();         // host tests only: frees everything, published or retired
bool     CgStoreReady();           // any thread
unsigned CgStoreGen();             // any thread
void     CgStoreNewWorld();        // main thread, at a save load: every save and live over goes stale

// Indexing. CgExteriorIndex, CgNodeKey, CgNodeDir, CgNodeIndex: any thread, pure. CgIndexOfUid and
// CgInsertInterior: main thread only (the interior table's one writer and reader).
int      CgExteriorIndex(int gx, int gy);      // -1 out of range
// An exterior uid x | (y << 8) (bits above 15 zero, x and y below 64) -> gy * 64 + gx; otherwise
// the interior table's directory index, or -1 when the uid is not in it.
int      CgIndexOfUid(int uid);
// An interior uid's directory index, inserting it when new; -1 when the probe run is full, or when
// the uid decodes as an exterior (counted uidClash).
int      CgInsertInterior(int uid);
unsigned CgNodeKey(int dirIndex, int node);
int      CgNodeDir(unsigned key);
int      CgNodeIndex(unsigned key);

// Publication of heap blocks. A writer moves the entry's epoch from even e to e + 1 with one
// compare-exchange (the take), swaps the pointer, and releases with e + 2; the old block goes onto the
// retire stack. The writers are the builder thread and the main thread. The builder retries a busy
// entry until it is free (a take is held across one pointer swap only); the main thread's own
// publishes (promotion and the hand-off drain) retry a bounded number of times, then keep the block
// for the next promotion (CGP_DEFERRED, counted deferred). A refused block goes onto the retire stack
// too, so no caller frees a block it handed to a publish.
enum CgPublishResult { CGP_OK = 0, CGP_OUTRANKED, CGP_NO_SLOT, CGP_DEFERRED };
CgPublishResult CgPublishBase(int dirIndex, CgBlock* b);   // builder thread
CgPublishResult CgPublishOver(int dirIndex, CgBlock* b);   // builder thread or main thread

// Read. Main thread only (the only thread that frees): the current block, a current-generation
// over or else the base; false when the entry holds neither, or while a writer holds it (an odd
// epoch, or an epoch that moved during the read) for two tries. A search reads each node's block
// afresh at every expansion, so the builder may swap a block between two expansions of one search.
// A swap changes which arcs later expansions see, never what a read returns. Every block read stays
// valid until the main thread's next retire drain, which the frame step runs outside any search.
struct CgView { const CgBlock* block; };
bool CgRead(int dirIndex, CgView* out);

// Path thread: a free live buffer (a lock-free pop) stamped with the current generation and the
// next acquire sequence, NULL when none (counted liveEmpty); a buffer back to the pool (a lock-free
// push, any thread). CgLivePost stamps buf with slot and exchanges it into record `slot`; a buffer
// it replaces, not yet taken by the main thread, goes back to the pool (superseded). A slot outside
// [0, CG_LIVE_RECORDS) is refused, the buffer released (slotCap).
CgBlock* CgLiveAcquire();
void     CgLiveRelease(CgBlock* b);
void     CgLivePost(int slot, CgBlock* buf);

// Builder thread: an interior save block onto the main thread's hand-off stack (a lock-free push).
void     CgHandOffInterior(CgBlock* heapBlock);

// Main thread, from the frame step: frees retired heap blocks; drains the builder's hand-off
// (inserting each uid, publishing each block, freeing a stale one); takes up to max posted live
// records, dropping an older generation (stale), mapping the uid (CgIndexOfUid, else
// CgInsertInterior), copying the buffer into a heap block, returning the buffer and publishing the
// copy (noSlot when the uid has no index). CgPromoteLive first retries the deferred publishes,
// retiring one of an older generation (stale). Each returns the blocks it published.
void CgDrainRetired();
int  CgDrainHandOff();
int  CgPromoteLive(int max);

// Builder thread: a heap block from one section of an extracted tile, with arcsTrunc counted; NULL
// on an allocation failure.
CgBlock* CgBlockFromTile(const TileGraph& g, int section, int source, unsigned storeGen);

// Cross resolution at read time (pure over blocks; coarse_graph_cross.cpp). For node `node` of `a`,
// each border is resolved against its neighbour's block (neighbourOf returns it or NULL) in order:
//   1. no far block: no arc, counted crossNoBlock;
//   2. a far border carrying both faces, swapped (mirrored): its node is the target;
//   3. a far border toward a naming either face, its portal within CG_PORTAL_SLACK: its node, the nearest portal;
//   4. toward a live or base block only: the node whose box, widened by the slack, holds the portal;
//   5. otherwise no arc, counted crossDropped (steps 3 and 4 count crossOneSided).
// A border kept by step 3 or 4 may be false; the route is advisory and the engine's own search
// decides each leg. The counts are added once per call, so a node expanded twice counts twice.
// Borders to one (neighbour, node) collapse into one arc whose portal is the longest border edge's
// midpoint, whose cost is the world distance between the two centres and whose water is the target
// node's byte. Returns the arcs written, at most max.
struct CgResolved { int dirIndex; int node; float cost; int water; float portal[3]; float edgeA[3]; float edgeB[3]; };
typedef const CgBlock* (*CgNeighbourFn)(void* ctx, int uid, int* dirIndexOut);
int CgCrossArcs(const CgBlock* a, int node, CgNeighbourFn neighbourOf, void* ctx, CgResolved* out, int max);
// The class steps 2-5 give border br of a against the far block nb; CG_BORDER_DROPPED when nb is NULL.
enum CgBorderClass { CG_BORDER_MIRRORED = 0, CG_BORDER_ONE_SIDED, CG_BORDER_DROPPED };
int CgClassifyBorder(const CgBlock* a, const CgBorder& br, const CgBlock* nb);

// Host tests only: a callback run by CgPublishBase and CgPublishOver right after the take and before
// the pointer swap; NULL in the game.
void CgTestPauseAfterTake(void (*fn)(void* ctx), void* ctx);

// Counters, any thread (interlocked). unsearchable: nodes without a finite centre and arcs with a
// negative or NaN cost in the blocks built or promoted, which the coarse search skips. crossNoBlock,
// crossDropped and crossOneSided: CgCrossArcs's borders by outcome, summed over its calls.
struct CgStats { long publishes; long busy; long outranked; long noSlot; long uidClash; long liveAcquired;
                 long liveEmpty; long posted; long superseded; long slotCap; long stale; long promoted;
                 long handedOff; long retiredFreed; long readBusy; long unsearchable;
                 long crossNoBlock; long crossDropped; long crossOneSided; long deferred; };
void CgStatsGet(CgStats* out);
// One interlocked add per non-zero count.
void CgStatsAddCross(long noBlock, long dropped, long oneSided);

} // namespace planner

#endif
