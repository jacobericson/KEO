#ifndef KEO_FIXES_UNSTITCH_LAYOUT_H
#define KEO_FIXES_UNSTITCH_LAYOUT_H

#include <stddef.h>

// Offsets the cross-section un-stitch walks, read off the binary and matched
// against the Havok classes the game was built from.
//
// hkaiStreamingSet is {thisUid, oppositeUid, three hkArrays} = 56 bytes, and
// the array this walk uses is the graph connections: data at +24, size at +32,
// 16-byte records whose first int is the edge key and second the opposite node
// index.
//
// hkaiStreamingSet arrays live on hkaiDirectedGraphExplicitCost -- the original
// graph, a shared loaded resource -- not on the instance, which is why nothing
// here writes them.
//
// hkaiDirectedGraphInstance carries the section uid, the runtime id, the
// original graph and three consecutive hkArrays: m_nodeMap (ints, indexed by a
// record's opposite node index), m_instancedNodes (8-byte {startEdge, numEdges}
// pairs, indexed by the value read out of m_nodeMap) and m_ownedEdges (8-byte
// records biased by m_numOriginalEdges).

const size_t OFF_GI_NUM_ORIG_EDGES   = 40;
const size_t OFF_GI_UID              = 84;
const size_t OFF_GI_RUNTIME_ID       = 88;
const size_t OFF_GI_GRAPH            = 96;
const size_t OFF_GI_NODEMAP_DATA     = 104;
const size_t OFF_GI_NODEMAP_SIZE     = 112;
const size_t OFF_GI_INSTNODES_DATA   = 120;
const size_t OFF_GI_INSTNODES_SIZE   = 128;
const size_t OFF_GI_OWNEDEDGES_DATA  = 136;

// hkaiDirectedGraphInstance's copies of its graph's node array and count,
// made when the instance is initialised; m_nodeMap is sized to that count.
const size_t OFF_GI_ORIG_NODES       = 16;
const size_t OFF_GI_NUM_ORIG_NODES   = 24;

// hkaiDirectedGraphExplicitCost: m_nodes, whose count is the node-map size an
// instance of the graph gets.
const size_t OFF_GRAPH_NODES_DATA    = 32;
const size_t OFF_GRAPH_NODES_SIZE    = 40;
const size_t OFF_GRAPH_SETS_DATA     = 104;
const size_t OFF_GRAPH_SETS_SIZE     = 112;

const size_t SET_STRIDE              = 56;
const size_t OFF_SET_THIS_UID        = 0;
const size_t OFF_SET_OPP_UID         = 4;
const size_t OFF_SET_CONN_DATA       = 24;
const size_t OFF_SET_CONN_SIZE       = 32;

const size_t CONN_STRIDE             = 16;
const size_t OFF_CONN_EDGE_KEY       = 0;
const size_t OFF_CONN_OPP_NODE       = 4;

const size_t INSTNODE_STRIDE         = 8;
const size_t EDGE_STRIDE             = 8;
const size_t OFF_EDGE_FLAGS          = 2;   // byte; 0x40 marks a live cross-section edge
const size_t OFF_EDGE_KEY            = 4;
const unsigned char EDGE_FLAG_LIVE   = 0x40;

// The streaming collection's uid -> instance table: 48-byte slots, each holding
// the graph instance at +16.
const size_t OFF_COLL_SLOTS          = 32;
const size_t OFF_COLL_SLOT_COUNT     = 40;
const size_t COLL_SLOT_STRIDE        = 48;
const size_t OFF_COLL_SLOT_GRAPHINST = 16;

// The dying instance's runtime id is packed into the edge key at this shift.
const int EDGE_KEY_RUNTIME_SHIFT     = 22;

#endif // KEO_FIXES_UNSTITCH_LAYOUT_H
