#pragma once
#include <stdint.h>

// Exposes readiness_hook.cpp's ClassifyZoneReadiness -- the islandReadinessRule read
// (a try-lock scan of the world streaming collection for a zone's outdoor
// navmesh instance; never state+0x28, never +0x1E0 held together with
// +0x200) -- to other translation units that need one classification
// without the counters and caller-identity plumbing hook_isContentPending
// carries around it.
//
// Values mirror readiness_hook.cpp's own hooks_detail::ReadyClass
// enum, duplicated here under a different prefix so the two enums never
// collide by name when both files are built.
enum
{
	ZR_NO_SECTION        = 0,
	ZR_OUTDOOR_MISSING   = 1,
	ZR_BUILDINGS_PENDING = 2,   // the outdoor instance is in the world
	ZR_NOT_IN_WORLD      = 3,
	ZR_UNKNOWN           = 4
};

// Any thread: no allocation, no logging, never blocks (try-locks only).
// splitMap = false skips the +0x1E0 noSection/outdoorMissing split and
// answers ZR_NOT_IN_WORLD for both those cases.
// sectionMgr is the NavMesh (SectionManager, *RVA_GLOBAL_SECTION_MGR), never
// the zone manager; NULL answers ZR_UNKNOWN.
int ClassifyZoneReadiness(uintptr_t sectionMgr, const int* pos, bool splitMap);
