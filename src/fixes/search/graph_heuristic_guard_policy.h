// graph_heuristic_guard_policy.h - Layout and classification for the three sites inside the A*
// search's hierarchical heuristic that read a section's cluster-graph instance unchecked.
// No Windows header and no game pointer, so a host test can fabricate every object.
#ifndef KENSHI_ZONE_OPT_FIXES_GRAPH_HEURISTIC_GUARD_POLICY_H
#define KENSHI_ZONE_OPT_FIXES_GRAPH_HEURISTIC_GUARD_POLICY_H

#include <stddef.h>
#include "fixes/search/graph_position_guard_policy.h"  // the collection layout, GraphPositionVec4

const size_t OFF_HEUR_VISITOR       = 8;
const size_t OFF_HEUR_START_CLUSTER = 0x18;
const size_t OFF_HEUR_COARSE        = 0x180;
const size_t OFF_HEUR_GOAL_POINTS   = 0x240;
const size_t OFF_HVIS_COLLECTION    = 0;
const size_t OFF_HVIS_FALLBACK      = 8;
const size_t OFF_HVIS_SLOT_ADJACENT = 8;    const size_t OFF_HVIS_SEC_ADJACENT = 0x68;
const size_t OFF_HVIS_SLOT_SEED     = 16;   const size_t OFF_HVIS_SEC_SEED     = 0x6C;
const size_t OFF_HVIS_SLOT_CENTRE   = 24;   const size_t OFF_HVIS_SEC_CENTRE   = 0x70;
const size_t OFF_COARSE_OPEN_COUNT  = 0x28;
const size_t OFF_COARSE_OPEN_CAP    = 0x2C;
const unsigned GH_RET_HEURISTIC_INIT = 0xDA69A1u;
const unsigned GH_RET_PATH_EXISTS    = 0xDA4713u;

enum GraphHeuristicSite { GH_SITE_ADJACENT = 0, GH_SITE_CENTRE, GH_SITE_SEED };
enum GraphHeuristicArm
{
	GH_RUN = 0,             // every instance the site would read is in place
	GH_UNJUDGED,            // no visitor, or the collection has no instance array
	GH_EARLY_RETURN,        // the seed returns before any read
	GH_BAD_SECTION,         // a key names a section past the collection's count
	GH_NO_COLLECTION,       // no collection and no fallback instance
	GH_NO_INSTANCE          // the looked-up or cached instance is NULL
};
struct GraphHeuristicCall
{
	const void* collection;
	unsigned key;           // the key that failed (or the last one read)
	unsigned section;
	GraphHeuristicArm arm;
};

// 0xDA6A30 and 0xDA6690: the one key through the site's own slot, section field and
// no-collection rule, exactly as the site reads it.
void InspectGraphHeuristicKey(const void* visitor, GraphHeuristicSite site, unsigned key, GraphHeuristicCall* out);
// 0xDA5FE0: the early return, then the start key and every goal key other than 0xFFFFFFFF.
void InspectGraphHeuristicSeed(const void* coarseSearch, const void* visitor, const unsigned* goalKeys,
                               unsigned count, unsigned startKey, GraphHeuristicCall* out);
bool GraphHeuristicArmFires(GraphHeuristicArm arm);   // BAD_SECTION, NO_COLLECTION, NO_INSTANCE
// Only the heuristic init's seed may turn the heuristic Euclidean; pathExists's context is not a heuristic.
bool GraphHeuristicSeedWritesStartCluster(unsigned returnRva, bool inExe);
// The centre a failed 0xDA6690 reports: goal 0's position, or the far sentinel without one.
GraphPositionVec4 GraphHeuristicCentreFallback(const GraphPositionVec4* goal0);

#endif
