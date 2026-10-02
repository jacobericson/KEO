// cluster_cross_cost_policy.h - The cross-tile cluster-graph link cost, rewritten from the two
// instances' world-frame cluster centres. Pure over the instances' memory: no Windows header
// and no game pointer type, so a host test fabricates both instances and the collection.
#ifndef KEO_FIXES_CLUSTER_CROSS_COST_POLICY_H
#define KEO_FIXES_CLUSTER_CROSS_COST_POLICY_H

#include <stddef.h>

const size_t OFF_GI_POSITIONS       = 48;
const size_t OFF_GI_BASE_EDGE_COUNT = 40;
const size_t OFF_GI_SECTION         = 88;
const size_t OFF_GI_OWNED_MAP       = 104;
const size_t OFF_GI_OWNED_MAP_COUNT = 112;
const size_t OFF_GI_OWNED_NODES     = 120;
const size_t OFF_GI_OWNED_NODE_COUNT = 128;
const size_t OFF_GI_OWNED_EDGES     = 136;
const size_t OFF_GI_OWNED_EDGE_COUNT = 144;
const size_t OFF_GI_ROW0            = 208;
const size_t OFF_GI_ROW1            = 224;
const size_t OFF_GI_ROW2            = 240;
const size_t OFF_GI_TRANSLATION     = 256;
const size_t OFF_CCC_INSTANCE_DATA  = 32;
const size_t OFF_CCC_INSTANCE_COUNT = 40;
const size_t SIZE_CCC_INSTANCE_INFO = 48;
const size_t OFF_CCC_INFO_GRAPH     = 16;
const unsigned short CROSS_EDGE_FLAG = 0x40;
const float CROSS_COST_SCALE        = 1.0039062f;

struct CrossCostCounts { long links; long rewritten; long skipped; };

// |T_a * pos_a[nodeA] - T_b * pos_b[nodeB]|, each position graph-local.
float CrossCostWorldDistance(const void* instA, unsigned nodeA, const void* instB, unsigned nodeB);
// The hkHalf the engine stores: the top 16 bits of d * CROSS_COST_SCALE.
unsigned short CrossCostHalf(float d);
// Every cross owned edge of inst whose target section differs from inst's own: its cost and its
// reciprocal's in the neighbour, rewritten; a link whose bound or NULL check fails is skipped
// and counted. A link whose neighbour has no reciprocal edge keeps its own side rewritten and
// is counted skipped too. Idempotent. Adds to *out.
void CrossCostRewrite(void* inst, void* coll, CrossCostCounts* out);

#endif
