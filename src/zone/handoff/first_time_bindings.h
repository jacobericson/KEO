// first_time_bindings.h - First-load prediction bindings and town-list layout.
// Included through game.h.

#ifndef KEO_FIRST_TIME_BINDINGS_H
#define KEO_FIRST_TIME_BINDINGS_H

#include "game/klib_members.h"
#include "base/core.h"

// ---- First-time prediction (zone_life.cpp) ----
// The test that says, before the mod's processContent, whether the zone's
// finalize will read it as first-time (content+0xA8). Called, not hooked; both
// KenshiLib 0.5.0 exports (klib_bindings.cpp cross-checks the addresses).
//   SaveFileSystem::getSingleton 0x37DD80: the SaveFileSystem* that
//     ZoneMapContent::_activate reads inline (qword_14212DC08).
//   SaveFileSystem::fileExists 0x470A70: `bool (this, const std::string& name)`,
//     the same fileSystem (+0x88) map lookup SaveFileSystem::readFile 0x470C20
//     makes; readFile returns "" when it misses, and _activate's
//     GameDataContainer::load of "" fails, which is its first-time test.
//     ZoneManager::checkZoneFiles 0xA0B020 calls it with the key
//     "zone/zone.<x>.<y>.zone" (x = ZoneMap::coordinates.x).
// contentManager 0x21330A0 (KenshiLib shou.townList, s_globals[8]): TownList*,
// whose townsByZone[x][y] (+0xB8, lektor<hand>, 24 bytes each) is the list the
// finalize's repopulate test and collectBuildings' barfly pass both read.
const size_t RVA_SFS_GET_SINGLETON = 0x37DD80;
const size_t RVA_SFS_FILE_EXISTS   = 0x470A70;
const size_t RVA_GLOBAL_TOWN_LIST  = 0x21330A0;
const size_t OFF_TOWNLIST_BY_ZONE  = 0xB8;
KLIB_ASSERT_OFFSET(TownList_townsByZone, OFF_TOWNLIST_BY_ZONE);
typedef void* (*sfsGetSingleton_t)();
typedef bool  (__fastcall *sfsFileExists_t)(void* sfs, const std::string* name);
extern sfsGetSingleton_t fn_sfsGetSingleton;
extern sfsFileExists_t   fn_sfsFileExists;
// ---- end first-time prediction ----

#endif // KEO_FIRST_TIME_BINDINGS_H
