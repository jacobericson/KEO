#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "game/klib_bindings.h"
#include "game/klib_address_policy.h"
#include "game/klib_dethunk_policy.h"
#include <Windows.h>
#include <stdio.h>
#include "base/klib_include.h"
#include "game/klib_platform_policy.h"
#include <core/Functions.h>
// Correct tags must precede upstream class forward declarations: MSVC
// encodes struct/class in imported names. These match the DLL exports.
struct EdgePathNode;
struct EdgeCache;
struct HavokCharacterMessage;
struct NavMeshSector;   // getSector/loadZone/deleteMesh export `PEAUNavMeshSector`
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/ZoneManager.h>
#include <kenshi/ZoneMapContent.h>
#include <kenshi/NavMesh.h>
#include <kenshi/NavMeshGenerator.h>
#include <kenshi/HavokCharacter.h>
#include <kenshi/PhysicsActual.h>
#include <kenshi/HandleManager.h>
#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharBody.h>
#include <kenshi/GameWorld.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/ResourceLoader.h>
#include <kenshi/Platoon.h>
#include <kenshi/Town.h>
#include <kenshi/SaveManager.h>
#include <kenshi/SaveFileSystem.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Faction.h>
#include <kenshi/FoliageSystem.h>
#include <kenshi/util/UtilityT.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Animation/AnimationClass.h>
#include <kenshi/NavInstance.h>
#include <stddef.h>
// The two NavInstance fields nm_nbr_seeds.cpp reads through the raw offsets
// in nbr_seed_bindings.h (OFF_NAVINST_MESH / OFF_NAVINST_TEMP).
static_assert(offsetof(NavInstance, mesh) == 0x08, "NavInstance::mesh drifted from nbr_seed_bindings.h OFF_NAVINST_MESH");
static_assert(offsetof(NavInstance, temp) == 0x45, "NavInstance::temp drifted from nbr_seed_bindings.h OFF_NAVINST_TEMP");
#include "base/klib_include_end.h"

// Official Globals.h is isolated in klib_globals.cpp: its FoliageSystem
// namespace forward declaration conflicts with the actual class header.
uintptr_t KlibGlobalAddress(uintptr_t rva);
uintptr_t KlibWeatherAddress(bool mainThread);

namespace klib_bindings_detail
{
// Our own image extent and import address table.
// SelfImage can first resolve on any caller thread; its plain volatile
// metadata stores precede base. Resolver callers read these immutable image
// values; racing initializers compute the same fields. No reset or changing
// set is published, and each aligned scalar is untorn.
static volatile uintptr_t s_selfBase = 0;
static volatile size_t s_selfSize = 0;
static volatile size_t s_selfIatOffset = 0;
static volatile size_t s_selfIatSize = 0;

static bool SelfImage(uintptr_t* base, size_t* size, size_t* iatOffset, size_t* iatSize)
{
	if (!s_selfBase)
	{
		HMODULE self = NULL;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)&KlibRealAddressOf, &self) || !self)
			return false;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
		const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const char*)self + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY& iat = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
		s_selfIatOffset = iat.VirtualAddress;
		s_selfIatSize = iat.Size;
		s_selfSize = nt->OptionalHeader.SizeOfImage;
		s_selfBase = (uintptr_t)self;
	}
	*base = s_selfBase;
	*size = s_selfSize;
	*iatOffset = s_selfIatOffset;
	*iatSize = s_selfIatSize;
	return true;
}
}
using namespace klib_bindings_detail;

intptr_t KlibRealAddressOf(void* function)
{
	uintptr_t address = (uintptr_t)function;
	uintptr_t base = 0;
	size_t size = 0, iatOffset = 0, iatSize = 0;
	if (SelfImage(&base, &size, &iatOffset, &iatSize))
		KlibDethunk(address, base, (const unsigned char*)base, size, iatOffset, iatSize, &address);
	return KenshiLib::GetRealAddress((void*)address);
}

namespace klib_bindings_detail
{
// Main plugin/profiler startup initializes this registry before any hooks
// can read KlibAddress on another thread. Resolve fills entries/globals, then
// successful InitKlibBindings sets the plain ready flag; failed startup can
// retry before installing hooks. No reset after success and no concurrent
// mutation: behavior readers use the immutable table, not a torn set.
static KlibAddressEntry s_entries[160];
static size_t s_count = 0;
static uintptr_t s_base = 0;
static bool s_ready = false;
static bool s_overflow = false;
static uintptr_t s_globals[9] = { 0 };

static void Add(const char* name, uintptr_t legacy, uintptr_t impl, uintptr_t resolved)
{
	if (s_count == sizeof(s_entries) / sizeof(s_entries[0]))
	{
		s_overflow = true;
		return;
	}
	KlibAddressEntry& entry = s_entries[s_count++];
	entry.name = name;
	entry.legacyRva = legacy;
	entry.implementationRva = impl;
	entry.resolved = resolved;
}
template<typename T>
static void Bind(const char* name, uintptr_t legacy, uintptr_t impl, T function)
{
	Add(name, legacy, impl, (uintptr_t)KlibRealAddress(function));
}
static void Resolve()
{
	// Legacy aliases are documented game jump thunks, not runtime E9 scans.
	// This normalizes the baseline without following another plugin's hook.
	Bind("ForgottenGUI::showLoadingMessage", 0x6E9800, 0x6E9800, &ForgottenGUI::showLoadingMessage);
	Bind("NavMesh::isLoaded(coords)", 0x3AB4F0, 0x3AB4F0, static_cast<bool (NavMesh::*)(const iVector2&)>(&NavMesh::isLoaded));
	Bind("ZoneManager::updateGPUSafeThread", 0x46FC9, 0xA11DA0, &ZoneManager::updateGPUSafeThread);
	Bind("ZoneManager::processLoading", 0xA0E950, 0xA0E950, &ZoneManager::processLoading);
	Bind("PlayerInterface::addOrderSelectedCharacters", 0x7F9280, 0x7F9280, &PlayerInterface::addOrderSelectedCharacters);
	Bind("ZoneMap::_activate", 0x16243, 0xA0D6A0, &ZoneMap::_activate);
	Bind("NavMesh::create", 0x2D7B8, 0x3ABF00, &NavMesh::create);
	Bind("ZoneMap::getAreaSector", 0x26BC0, 0x8F3E00, &ZoneMap::getAreaSector);
	Bind("ZoneMapContent::createLoadedItems", 0xD5BC, 0x9FC720, &ZoneMapContent::createLoadedItems);
	Bind("TownBase::delayedSpawningChecks", 0x2E073, 0x927540, &TownBase::delayedSpawningChecks);
	Bind("PhysicsInterface::setQueuesAreClear", 0x579460, 0x579460, &PhysicsInterface::setQueuesAreClear);
	Bind("PlatoonHandleContainerList::getCharacterOrPlatoon", 0x3E419, 0x4FDF90, &PlatoonHandleContainerList::getCharacterOrPlatoon);
	Bind("hand::getCharacter", 0x7974F0, 0x7974F0, &hand::getCharacter);
	Bind("NavMeshGenerator::generateTaskBT", 0x3CBE60, 0x3CBE60, &NavMeshGenerator::generateTaskBT);
	Bind("NavMeshGenerator::updateBT", 0x3CE030, 0x3CE030, &NavMeshGenerator::updateBT);
	Bind("NavMeshGenerator::stitchExterior", 0x39810, 0x3CB700, &NavMeshGenerator::stitchExterior);
	Bind("NavMeshGenerator::stitchInterior", 0x3CBAB0, 0x3CBAB0, &NavMeshGenerator::stitchInterior);
	Bind("NavMeshGenerator::splice", 0x1C418, 0x3CA290, &NavMeshGenerator::splice);
	Bind("NavMeshGenerator::TaskQueue::push", 0x2EEBA, 0x3C6620, &NavMeshGenerator::TaskQueue::push);
	Bind("hkMemoryAllocator::_DESTRUCTOR", 0xBA9580, 0xBA9580, &hkMemoryAllocator::_DESTRUCTOR);
	Bind("NavMesh::shutdown", 0x3AAE90, 0x3AAE90, &NavMesh::shutdown);
	Bind("NavMesh::directPath", 0x3AA950, 0x3AA950, &NavMesh::directPath);
	Bind("NavMesh::pathExists(keys)", 0x3A5B00, 0x3A5B00, static_cast<bool (NavMesh::*)(unsigned int, unsigned int)>(&NavMesh::pathExists));
	Bind("NavMesh::findPath", 0x3AABF0, 0x3AABF0, &NavMesh::findPath);
	Bind("HavokCharacter::requestPath", 0x145CB0, 0x145CB0, &HavokCharacter::requestPath);
	Bind("NavMesh::postMessage", 0x3AAEF0, 0x3AAEF0, &NavMesh::postMessage);
	Bind("ZoneMap::isInIsland", 0x2FCE8, 0xA07EB0, &ZoneMap::isInIsland);
	Bind("ZoneManager::getIsland", 0xA09AE0, 0xA09AE0, &ZoneManager::getIsland);
	Bind("ZoneManager::_calculateIslands", 0xA09520, 0xA09520, &ZoneManager::_calculateIslands);
	Bind("NavMesh::getZoneEdge", 0x3A39C0, 0x3A39C0, &NavMesh::getZoneEdge);
	Bind("NavMesh::isLoaded(zone)", 0x3AC810, 0x3AC810, static_cast<bool (NavMesh::*)(ZoneMap*)>(&NavMesh::isLoaded));
	Bind("GameWorld::destroy(MovableObject)", 0x799BE0, 0x799BE0, static_cast<void (GameWorld::*)(Ogre::MovableObject*)>(&GameWorld::destroy));
	Bind("NavMesh::createEdgePath", 0x3A9F20, 0x3A9F20, &NavMesh::createEdgePath);
	Bind("NavMesh::update", 0x3AE350, 0x3AE350, &NavMesh::update);
	Bind("Character::isPlayerCharacter", 0x790B30, 0x790B30, &Character::isPlayerCharacter);
	Bind("ZoneMap::isTerrainCollisionLoaded", 0xA08E10, 0xA08E10, &ZoneMap::isTerrainCollisionLoaded);
	Bind("ZoneManager::deactivateAllActiveZones", 0x36C1E0, 0x36C1E0, &ZoneManager::deactivateAllActiveZones);
	Bind("ZoneManager::deactivateZoneMap", 0x365ED, 0xA09BB0, &ZoneManager::deactivateZoneMap);
	// The zone-lifecycle first-time prediction (game.h). Exported since 0.5.0.
	Bind("SaveFileSystem::getSingleton", 0x37DD80, 0x37DD80, &SaveFileSystem::getSingleton);
	Bind("SaveFileSystem::fileExists", 0x470A70, 0x470A70, &SaveFileSystem::fileExists);
	// Cancel hooks (bound like the other covered hook targets). All three
	// exported since 0.5.0.
	Bind("PlayerInterface::stopCharactersMovement", 0x7F58D0, 0x7F58D0, &PlayerInterface::stopCharactersMovement);
	Bind("PlayerInterface::addJobSelectedCharacters", 0x7F4EF0, 0x7F4EF0, &PlayerInterface::addJobSelectedCharacters);
	Bind("PlayerInterface::addTaskNearestSelectedCharacter", 0x7FA2D0, 0x7FA2D0, &PlayerInterface::addTaskNearestSelectedCharacter);
	// The neighbour-seed hook target (game.h) and the sector lookup its
	// classification makes. Bound in every build like the other covered
	// targets; both exported since 0.5.0 (KenshiLib's Steam address table, not its GOG header comments).
	Bind("NavMeshGenerator::getSeedPointsFromAdjacentZone", 0x3C96E0, 0x3C96E0, &NavMeshGenerator::getSeedPointsFromAdjacentZone);
	Bind("NavMesh::getSector(coords)", 0x3AB1F0, 0x3AB1F0, static_cast<NavMeshSector* (NavMesh::*)(const iVector2&, bool)>(&NavMesh::getSector));
	// The shipped-tile load stitchUnloadedZone makes, and the zone lookup.
	// All 0.5.0 exports.
	Bind("NavMesh::getFilename", 0x3A6230, 0x3A6230, &NavMesh::getFilename);
	Bind("NavMesh::loadZone", 0x3A7F10, 0x3A7F10, &NavMesh::loadZone);
	Bind("NavMesh::deleteMesh(sector)", 0x3AC8B0, 0x3AC8B0, static_cast<void (NavMesh::*)(NavMeshSector*)>(&NavMesh::deleteMesh));
	Bind("NavMeshGenerator::lockZone", 0x3C73B0, 0x3C73B0, &NavMeshGenerator::lockZone);
	Bind("NavMeshGenerator::unlockZone", 0x3BF560, 0x3BF560, &NavMeshGenerator::unlockZone);
	Bind("ZoneManager::getZoneMap(int,int)", 0xA07C10, 0xA07C10, static_cast<ZoneMap* (ZoneManager::*)(int, int)>(&ZoneManager::getZoneMap));

	// Virtual calls keep dynamic dispatch; _NV_ is used only for parity.
	Bind("ZoneMapContent::_NV_update", 0x9FF730, 0x9FF730, &ZoneMapContent::_NV_update);
	Bind("Character::_NV_playerMoveOrderDefault", 0x5D1820, 0x5D1820, &Character::_NV_playerMoveOrderDefault);
	Bind("AbstractMovementBase::_NV_getPosition", 0x664640, 0x664640, &AbstractMovementBase::_NV_getPosition);
	Bind("CharMovement::_NV_pathOk", 0x65DD70, 0x65DD70, &CharMovement::_NV_pathOk);
	Bind("CharMovement::_NV_pathFailed", 0x65DDA0, 0x65DDA0, &CharMovement::_NV_pathFailed);
	Bind("CharMovement::_NV_isDestinationReached", 0x65E320, 0x65E320, &CharMovement::_NV_isDestinationReached);
	Bind("AbstractMovementBase::getDestination", 0x65DD40, 0x65DD40, &AbstractMovementBase::getDestination);

	// Profiler. Existing hook ABI adapters and orig_* trampolines stay intact.
	Bind("ResourceLoader::updateMT", 0x44BBA0, 0x44BBA0, &ResourceLoader::updateMT);
	Bind("Platoon::activate", 0x7EB250, 0x7EB250, &Platoon::activate);
	Bind("GameWorld::_NV_mainLoop_GPUSensitiveStuff", 0x787E70, 0x787E70, &GameWorld::_NV_mainLoop_GPUSensitiveStuff);
	Bind("GameDataContainer::load", 0x6C0800, 0x6C0800, &GameDataContainer::load);
	Bind("ZoneManager::activateZoneMap", 0xA0E1B0, 0xA0E1B0, static_cast<bool (ZoneManager::*)(ZoneMap*, iVector2, int, ZoneActivationType, float)>(&ZoneManager::activateZoneMap));
	Bind("ZoneManager::setup", 0xA12880, 0xA12880, &ZoneManager::setup);
	Bind("NavMesh::generate", 0x3AB700, 0x3AB700, static_cast<void (NavMesh::*)(ZoneMap*)>(&NavMesh::generate));

	// Frame-audit entry detours. Virtual implementation addresses use _NV_.
	Bind("PhysicsActual::_NV_backThreadUpdate", 0x7DC4C0, 0x7DC4C0, &PhysicsActual::_NV_backThreadUpdate);
	Bind("PhysicsActual::_NV_updateUT", 0x4CD040, 0x4CD040, &PhysicsActual::_NV_updateUT);
	Bind("ZoneMap::update", 0xA0A960, 0xA0A960, &ZoneMap::update);
	Bind("CharBody::_NV_update", 0x5C6290, 0x5C6290, &CharBody::_NV_update);
	Bind("CharMovement::_NV_update", 0x65F510, 0x65F510, &CharMovement::_NV_update);

	// Covered direct probe callees. Interior E8 checks keep their original
	// targetRva: the call encodes the thunk, while we compare implementations.
	Bind("GameWorld::processThreadMessages", 0x31372, 0x785260, &GameWorld::processThreadMessages);
	Bind("GameWorld::processKillList", 0x489F5, 0x79CD00, &GameWorld::processKillList);
	Bind("GameWorld::charsUpdateUT", 0x28D5D, 0x785EA0, &GameWorld::charsUpdateUT);
	Bind("SaveManager::execute", 0x17607, 0x47BAA0, &SaveManager::execute);
	Bind("GameWorld::threadSafeRagdollUpdates", 0x54089, 0x7D17E0, &GameWorld::threadSafeRagdollUpdates);
	Bind("PlayerInterface::update", 0x384FB, 0x800A80, &PlayerInterface::update);
	Add("WeatherSystem::updateMT", 0x2A810, 0x9E9540, KlibWeatherAddress(true));
	Bind("ZoneManager::updateMainThread", 0xFA1A, 0xA11B70, &ZoneManager::updateMainThread);
	Bind("RootObjectFactory::mainThreadUpdate", 0x18CD7, 0x582880, &RootObjectFactory::mainThreadUpdate);
	Bind("GameWorld::charsUpdate", 0x4B628, 0x7862F0, &GameWorld::charsUpdate);
	Bind("GameWorld::charsUpdatePaused", 0xB4C4, 0x7866F0, &GameWorld::charsUpdatePaused);
	Bind("FactionManager::updateMT", 0x234A7, 0x2E74B0, &FactionManager::updateMT);
	Bind("ForgottenGUI::update", 0x2670B, 0x6E98D0, &ForgottenGUI::update);
	Bind("ThreadClass::startRunning", 0x49071, 0x3BED70, &ThreadClass::startRunning);
	Bind("FoliageSystem::update", 0x5FB0, 0x6CC120, &FoliageSystem::update);
	Bind("PlayerInterface::mouseScan", 0x3BDAE, 0x7FF6B0, &PlayerInterface::mouseScan);
	Bind("UtilityT::traceAll", 0x1C79C, 0x9B28D0, &UtilityT::traceAll);
	Bind("UtilityT::trace", 0x2E2C1, 0x9B5BC0, &UtilityT::trace);
	Bind("ParticlePool::update", 0x8F2B, 0x40BA60, &ParticlePool::update);
	Bind("ZoneManager::updateRendertimeThread", 0x202CF, 0xA0E400, &ZoneManager::updateRendertimeThread);
	Bind("TownList::updateTownsDiscovery", 0x455A2, 0x92E7E0, &TownList::updateTownsDiscovery);
	Bind("FactionManager::updateThreaded", 0x2464, 0x2E7550, &FactionManager::updateThreaded);
	Bind("Character::updateOnScreenCheck", 0x3560C, 0x5C94D0, &Character::updateOnScreenCheck);
	Add("WeatherSystem::updateBT", 0x33A73, 0x9E80A0, KlibWeatherAddress(false));
	Bind("AITaskSytem::update", 0xF65F, 0x50CE90, &AITaskSytem::update);
	Bind("AnimationClass::updateThreaded", 0x207B1, 0x5B3CE0, &AnimationClass::updateThreaded);
	Bind("PhysicsActual::threadJunkPreBT", 0xA78B, 0x4CBB90, &PhysicsActual::threadJunkPreBT);
	Bind("PhysicsActual::threadJunkPostBT", 0x17A58, 0x4CCEB0, &PhysicsActual::threadJunkPostBT);

	const uintptr_t globals[] = { 0x21330B0, 0x21330C8, 0x2133560, 0x2133630,
		0x2132440, 0x21322B0, 0x21322B8, 0x2133098, 0x21330A0 };
	const char* names[] = { "ou", "ou.physics", "ou.navmesh", "ou.player",
		"options", "au", "au.render", "shou/areasList", "shou.townList" };
	for (size_t i = 0; i < sizeof(globals)/sizeof(globals[0]); ++i)
	{
		s_globals[i] = KlibGlobalAddress(globals[i]);
		Add(names[i], globals[i], globals[i], s_globals[i]);
	}
}
}
using namespace klib_bindings_detail;

bool InitKlibBindings(uintptr_t base, KlibLogFn log)
{
	if (s_ready) return s_base == base;
	KenshiLib::BinaryVersion ver = KenshiLib::GetKenshiVersion();
	if (!KlibSupportsBinary(base, ver.GetVersion(), ver.GetPlatform()))
	{
		if (log) log("[KLib] REFUSED: only Steam 1.0.65 is supported; no hooks installed");
		return false;
	}
	s_base = base;
	s_count = 0;
	s_overflow = false;
	Resolve();
	bool ok = !s_overflow;
	if (s_overflow && log) log("[KLib] address registry overflow; initialization refused");
	for (size_t i = 0; i < s_count; ++i)
	{
		if (!KlibAddressesMatch(base, &s_entries[i], 1, NULL))
		{
			char line[384];
			_snprintf_s(line, sizeof(line), _TRUNCATE,
				"[KLib] MISMATCH %s ours=0x%I64X klib=0x%I64X (legacyRva=0x%I64X)",
				s_entries[i].name, (unsigned __int64)(base + s_entries[i].implementationRva),
				(unsigned __int64)s_entries[i].resolved, (unsigned __int64)s_entries[i].legacyRva);
			if (log) log(line);
			ok = false;
		}
	}
	if (!ok) return false;
	s_ready = true;
	char line[128];
	_snprintf_s(line, sizeof(line), _TRUNCATE, "[KLib] step=%d klib=ok(%u addresses)", 5, (unsigned)s_count);
	if (log) log(line);
	return true;
}

uintptr_t KlibAddress(uintptr_t base, uintptr_t legacyRva)
{
	if (!s_ready || base != s_base) return 0;
	// Hot per-frame/per-character globals bypass the function-table scan.
	switch (legacyRva)
	{
	case 0x21330B0: return s_globals[0];
	case 0x21330C8: return s_globals[1];
	case 0x2133560: return s_globals[2];
	case 0x2133630: return s_globals[3];
	case 0x2132440: return s_globals[4];
	case 0x21322B0: return s_globals[5];
	case 0x21322B8: return s_globals[6];
	case 0x2133098: return s_globals[7];
	case 0x21330A0: return s_globals[8];
	}
	uintptr_t resolved = KlibFindAddress(s_entries, s_count, legacyRva);
	if (resolved) return resolved;
	return base + legacyRva;
}

void KlibProcessZoneContent(void* content)
{
	(void)static_cast<ZoneMapContent*>(content)->update();
}
void KlibPlayerMoveOrder(uintptr_t character, void* building, void* subject, const float* destination)
{
	((Character*)character)->playerMoveOrderDefault(static_cast<Building*>(building),
		static_cast<RootObject*>(subject), *reinterpret_cast<const Ogre::Vector3*>(destination));
}
const float* KlibMovementPosition(void* movement)
{
	return &static_cast<AbstractMovementBase*>(movement)->getPosition().x;
}
bool KlibMovementPathOk(void* movement)
{
	return static_cast<AbstractMovementBase*>(movement)->pathOk();
}
bool KlibMovementPathFailed(void* movement)
{
	return static_cast<AbstractMovementBase*>(movement)->pathFailed();
}
bool KlibMovementDestinationReached(void* movement)
{
	return static_cast<AbstractMovementBase*>(movement)->isDestinationReached();
}
void KlibMovementDestination(void* movement, float* destination)
{
	// Nonvirtual call through the same verified implementation as the gate.
	Ogre::Vector3 value = static_cast<AbstractMovementBase*>(movement)->getDestination();
	destination[0] = value.x;
	destination[1] = value.y;
	destination[2] = value.z;
}
void* KlibBlockAlloc(uintptr_t allocator, int size)
{
	return ((hkMemoryAllocator*)allocator)->blockAlloc(size);
}
void KlibBlockFree(uintptr_t allocator, void* memory, int size)
{
	((hkMemoryAllocator*)allocator)->blockFree(memory, size);
}

void* KlibSelectedCharacter(const void* selectedHand)
{
	if (!selectedHand) return NULL;
	return static_cast<const hand*>(selectedHand)->getCharacter();
}
