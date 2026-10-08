#ifndef KEO_NPC_SEARCH_PROBE_POLICY_H
#define KEO_NPC_SEARCH_PROBE_POLICY_H

// Bounded, read-only face metadata for character-search diagnostics. No game header.
#include <stddef.h>

const unsigned NSP_NO_KEY              = 0xFFFFFFFFu;
const int      NSP_MAX_SECTIONS        = 4096;    // a larger slot count is not a collection
const size_t NSP_IN_START_FACE  = 0x30;
const size_t NSP_IN_GOAL_KEYS   = 0x38;   // hkArray<unsigned>: the data, then the int count at +8
const size_t NSP_COLL_INSTANCES   = 0x20;   // 48-byte records, the instance pointer first
const size_t NSP_COLL_COUNT       = 0x28;
const size_t NSP_INFO_STRIDE      = 48;
const size_t NSP_INST_ORIG_FACES  = 0x10;
const size_t NSP_INST_NUM_ORIG    = 0x18;
const size_t NSP_INST_ORIG_DATA   = 0x40;
const size_t NSP_INST_DATA_STRIDE = 0x48;
const size_t NSP_INST_FACE_MAP    = 0xE0;   // each array here: the data, then the int size at +8
const size_t NSP_INST_INST_FACES  = 0xF0;
const size_t NSP_INST_OWNED_FACES = 0x110;
const size_t NSP_INST_INST_DATA   = 0x160;
const size_t NSP_INST_OWNED_DATA  = 0x180;
const size_t NSP_FACE_BYTES       = 16;
const size_t NSP_FACE_CLUSTER     = 12;     // int16

unsigned NpcProbeClusterKey(unsigned faceKey, int cluster);
bool NpcProbeReadFace(const void* collection, unsigned faceKey, int* cluster, int* data);

#endif // KEO_NPC_SEARCH_PROBE_POLICY_H
