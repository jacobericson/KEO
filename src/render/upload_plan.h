#pragma once
#include "render/upload_shadow.h"

// The copy plan of one constant buffer: for each shader variable, the
// destination range and the constant definition whose physical index says
// where in the parameters' float or int array its bytes start. Resolving the
// definitions takes one map lookup per variable; the plan keeps them, so an
// upload is compared with the buffer's copy without any lookup. No Windows
// or game calls, so the host tests link it directly.

// Ogre::GpuConstantDefinition: the type at +0, the physical index (in 4-byte
// units) at +8.
static const size_t UPLOAD_DEF_TYPE       = 0x00;
static const size_t UPLOAD_DEF_PHYS_INDEX = 0x08;

// Whether s's plan was completed for these: the parameters' named-constant
// map, the program node's variable records and the map-deletion count.
bool UploadPlanMatches(const UploadShadow* s, const void* map, const void* vars,
                       const void* varsEnd, long mapGen);

// Appends a step (after UploadShadowTable::ReservePlan). False when the
// destination lies outside the copy or the reserved room is used up.
bool UploadPlanAdd(UploadShadow* s, size_t dst, size_t size, const void* def, int type, bool isFloat);

// Completes the plan for the given key (see UploadPlanMatches).
void UploadPlanCommit(UploadShadow* s, const void* map, const void* vars, const void* varsEnd, long mapGen);

// Records that no plan can be made for this key (too many variables, the
// byte cap, a variable outside the buffer), so it is not tried again until
// the key changes or the plan is dropped.
void UploadPlanRefuse(UploadShadow* s, const void* map, const void* vars, const void* varsEnd, long mapGen);
bool UploadPlanRefusedFor(const UploadShadow* s, const void* map, const void* vars,
                          const void* varsEnd, long mapGen);

enum UploadCompare
{
	UC_SAME,      // the copy was valid and every step's bytes equal it
	UC_DIFFERS,   // the copy now holds the new bytes; valid is unchanged
	UC_STALE      // a definition's type changed: plan and copy invalid
};

// Compares the bytes each step would copy (from floats or ints plus 4 times
// the definition's current physical index) with the copy, storing any that
// differ. Needs a valid plan.
UploadCompare UploadPlanCompare(UploadShadow* s, const char* floats, const char* ints);
