#include "game/klib_member_contract.h"
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "base/klib_include.h"
struct EdgePathNode;
struct EdgeCache;
struct HavokCharacterMessage;
#include <stddef.h>

#include <kenshi/GameWorld.h>
#include <kenshi/ZoneManager.h>
#include <kenshi/ZoneMapContent.h>
#include <kenshi/NavMesh.h>
#include <kenshi/NavMeshGenerator.h>
#include <kenshi/NavInstance.h>
#include <kenshi/Character.h>
#include <kenshi/CharStats.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharBody.h>
#include <kenshi/HavokCharacter.h>
#include <kenshi/AI/AI.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/AI/Blackboard.h>
#include <kenshi/Tasker.h>
#include <kenshi/Platoon.h>
#include <kenshi/Town.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/PhysicsActual.h>
#include <kenshi/Faction.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Renderer.h>
#include <kenshi/OptionsHolder.h>
#include <ogre/OgreRenderOperation.h>
typedef lektor<hand> HandLektor;
typedef MessageQueue<HavokCharacterMessage*> MsgQueue;
typedef hkArray<HavokCharacterMessage*,hkContainerHeapAllocator> HKArray;
typedef MainthreadStateReaderT<ZoneMap::StateT> ZoneState;
typedef MessageChain<TerrainSector*> TerrainChain;
typedef std::pair<std::string,bool> Compositor;

#define P(T,M,O) static_assert(offsetof(T,M)==O,#T "::" #M);
P(GameWorld,physics,0x18)
P(GameWorld,factionMgr,0x4A8)
P(GameWorld,navmesh,0x4B0)
P(GameWorld,player,0x580)
P(GameWorld,destroyListOE,0x680)
P(GameWorld,frameSpeedMult,0x700)
P(GameWorld,deathParade,0x708)
P(GameWorld,charUpdateListMain,0x750)
P(GameWorld,_AINonRenderThread,0x790)
P(GameWorld,killListPhase0,0x7D0)
P(GameWorld,zoneMgr,0x8B0)
P(GameWorld,paused,0x8B9)
P(RootObjectBase,data,0x40)
P(RootObjectBase,pos,0x48)
P(RootObjectBase,handle,0x58)
P(Character,_isLiterallyUnderMeleeAttackRightNowForSure,0x2B0)
P(Character,movement,0x640)
P(Character,body,0x648)
P(Character,ai,0x650)
P(Character,platoon,0x658)
P(AbstractMovementBase,officiallyStopped,0x8)
P(AbstractMovementBase,speedOrders,0x20)
P(AbstractMovementBase,currentlyMoving,0x24)
P(AbstractMovementBase,pos,0xC4)
P(AbstractMovementBase,destination,0xDC)
P(AbstractMovementBase,pathDestination,0xE8)
P(CharMovement,havokCharacter,0x320)
P(CharMovement,edgeTarget,0x368)
P(CharMovement,movingToEdge,0x370)
P(CharBody,combatClass,0x8)
P(CharBody,character,0x18)
P(CharBody,currentAction,0x68)
P(HavokCharacter,currentFace,0x14)
P(HavokCharacter,characterState,0x88)
P(HavokCharacter,pathState,0x90)
P(Tasker,taskData,0x70)
P(TaskData,key,0x44)
P(AI,platoon,0x10)
P(AI,taskSystemAI,0x20)
P(AI,sensoryData,0x28)
P(SensoryData,nearestEnemy,0x0)
P(SensoryData,threats,0x50)
P(SensoryData,totalThreatLevelPersonal,0x88)
P(SensoryData,numEnemies,0x94)
P(OrdersReceiver,orders,0x38)
P(OrdersReceiver,permajobs,0x88)
P(OrdersReceiver,currentGoal,0x1C0)
P(OrdersReceiver,currentGoalPriority,0x20C)
P(ActionDeque,list,0x8)
P(TaskMatch,taskData,0x0)
P(AITaskSytem,_taskCompletedFlag,0x26C)
P(AITaskSytem,_taskImpossibleFlag,0x26D)
P(Platoon,blackboard,0xF8)
P(Blackboard,requests,0x158)
P(PlayerInterface,camera,0x30)
P(PlayerInterface,selectedCharacters,0x208)
P(PlayerInterface,playerCharacters,0x2B0)
P(ZoneManager,justLoadedAGame,0x8)
P(ZoneManager,worldMap,0xC8)
P(ZoneManager,processingNewActiveZones,0x1680C8)
P(ZoneManager,activeZones,0x168108)
P(ZoneManager,centralZone,0x1681D0)
P(ZoneManager,loadingPhase,0x1681D8)
P(ZoneMap,mapContent,0x0)
P(ZoneMap,coordinates,0x18)
P(ZoneMap,island,0x20)
P(ZoneMap,stateT,0x88)
P(ZoneMap,terrainCollision,0xB8)
P(ZoneMap,activatedCountdown,0xC0)
P(ZoneMap,_generateNavMeshesFlag,0xCC)
P(ZoneMap,bounds,0xD0)
P(ZoneMap,center,0xF8)
P(ZoneMap,neighbors,0x128)
P(ZoneMap::StateT,_zoneBeingLoaded,0x0)
P(ZoneMap::StateT,_zoneIsLoaded,0x1)
P(RootObjectContainer,things,0x50)
P(ZoneMapContent,handleDummy,0xA0)
P(ZoneMapContent,activationFlag,0x108)
P(ZoneMapContent,items,0x110)
P(ZoneMapContent::HandleDummy,me,0x78)
P(PhysicsInterface,hullsToMake,0x1A0)
P(PhysicsInterface,hullsToDestroy,0x1F0)
P(PhysicsInterface,actorsToDestroy,0x298)
P(PhysicsInterface,terrainToLoad,0x2E8)
P(PhysicsInterface,_queuesClear,0x320)
P(PhysicsInterface,queuesClearMuto,0x328)
P(ThreadClass,threadHandle,0x8)
P(ThreadClass,_running,0x14)
P(ThreadClass,runMute,0x48)
P(ThreadClass,lockedWhileRunningMute,0x68)
P(NavMesh,world,0x88)
P(NavMesh,worldShiftEnabled,0xB0)
P(NavMesh,characterMessages,0xB8)
P(NavMesh,completedMessages,0xF8)
P(NavMesh,navMessages,0x138)
P(NavMesh,doorRequests,0x178)
P(NavMesh,pathRequests,0x1B8)
P(NavMesh,running,0x1C8)
P(NavMesh,cellSize,0x1D4)
P(NavMesh,worldShift,0x1D8)
P(NavMesh,mutex,0x1E0)
P(NavMesh,changeMutex,0x200)
P(NavMesh,sectors,0x220)
P(NavMesh,addList,0x270)
P(NavMesh,generator,0x290)
P(NavMeshGenerator,queue,0x88)
P(NavMeshGenerator,done,0xB8)
P(NavMeshGenerator,current,0xE8)
P(NavMeshGenerator,navmesh,0xF0)
P(NavMeshGenerator,settings,0x100)
P(NavMeshGenerator,doingStuff,0x109)
P(NavMeshGenerator,taskMutex,0x110)
P(NavMeshGenerator,seedPoints,0x138)
P(NavMeshGenerator::TaskQueue,front,0x0)
P(NavMeshGenerator::TaskQueue,back,0x8)
P(NavMeshGenerator::TaskQueue,mutex,0x10)
P(NavMeshGenerator::Task,zone,0x0)
P(NavMeshGenerator::Task,buildings,0x8)
P(NavMeshGenerator::Task,hash,0x20)
P(NavMeshGenerator::Task,bounds,0x30)
P(NavMeshGenerator::Task,mesh,0x48)
P(NavMeshGenerator::Task,output,0x50)
P(NavMeshGenerator::Task,flags,0x58)
P(NavMeshGenerator::Task,next,0x60)
P(NavMeshSeeds,seedPoints,0x0)
P(NavInstance,instance,0x28)
P(NavInstance,uid,0x3C)
P(NavInstance,hash,0x40)
P(FactionManager,participants,0x0)
P(Faction,activePlatoons,0x208)
P(CameraClass,altitude,0x60)
P(CameraClass,center,0x58)
P(CameraClass,centerBuilding,0xB0)
P(CameraClass,freeCameraMode,0xBF)
P(CameraClass,objectCurrentlyFollowing,0x28)
P(Renderer,scene,0x60)
P(OptionsHolder,VIEW_DISTANCE,0x18)
P(OptionsHolder,terrainDetail,0x1C)
P(OptionsHolder,grassRange,0x2C)
P(OptionsHolder,grassDensity,0x30)
P(OptionsHolder,foliageRange,0x34)
P(OptionsHolder,NPCRange,0x38)
P(OptionsHolder,smallBuildingRange,0x3C)
P(OptionsHolder,fancyShaders,0x41)
P(OptionsHolder,populationMult,0x48)
P(OptionsHolder,shadowMode,0x5C)
P(OptionsHolder,shadowQuality,0x60)
P(OptionsHolder,decalRange,0x68)
P(OptionsHolder,characterMultithreading,0x70)
P(OptionsHolder,showNames,0x78)
P(OptionsHolder,manyActiveZones,0x88)
P(OptionsHolder,distantTownRange,0x8C)
P(OptionsHolder,featureRange,0x90)
P(OptionsHolder,shadowRange,0x98)
P(OptionsHolder,reflectionMode,0xA0)
P(OptionsHolder,reflectionDistance,0xA4)
P(OptionsHolder,compositors,0xA8)
P(hand,type,0x8)
P(hand,container,0xC)
P(hand,containerSerial,0x10)
P(hand,index,0x14)
P(hand,serial,0x18)

P(HandLektor,count,8) P(HandLektor,maxSize,12) P(HandLektor,stuff,16)
P(MsgQueue,s,0) P(MsgQueue,root,8) P(MsgQueue,split,16) P(MsgQueue,back,24) P(MsgQueue,mutex,32)

P(HKArray,m_data,0) P(HKArray,m_size,8) P(HKArray,m_capacityAndFlags,12)
P(ZoneState,mainThreadData,40) P(ZoneState,backThreadData,42) P(ZoneState,swapMutex,8)
P(TerrainChain,mainThreadData,8) P(TerrainChain,backThreadData,32)
P(Compositor,second,40) P(Ogre::RenderOperation,numberOfInstances,24)
P(Ogre::Vector3,x,0) P(Ogre::Vector3,y,4) P(Ogre::Vector3,z,8)
P(Ogre::Aabb,mCenter,0) P(Ogre::Aabb,mHalfSize,12)
#define S(T,N) static_assert(sizeof(T)==N,#T " size");
S(ZoneMap,KLIB_ZONE_STRIDE) S(NavMeshGenerator,KLIB_SIZE_NavMeshGenerator) S(NavMeshGenerator::Task,104) S(NavInstance,72)
S(NavMesh,704) S(HandLektor,24) S(hand,32) S(HKArray,16) S(hkVector4f,16) S(Ogre::Vector3,12) S(Ogre::Aabb,24)
S(boost::shared_mutex,32) S(MsgQueue,64)  S(NavMeshGenerator::TaskQueue,48)
S(ZoneMap::StateT,2) S(std::string,40) S(Compositor,KLIB_SIZE_Compositor)

P(RootObject,rot,0xB0) P(GameData,name,0x28) P(GameData,stringID,0x58) P(hkVector4f,m_quad,0)
P(iVector2,x,0) P(iVector2,y,4)
S(Ogre::Quaternion,16)
typedef ogre_unordered_set<ZoneMap*>::type ZoneSet;
typedef boost::unordered::detail::set<ZoneSet::allocator_type,ZoneSet::value_type,ZoneSet::hasher,ZoneSet::key_equal>::table ZoneSetTable;
P(ZoneSetTable,bucket_count_,24) P(ZoneSetTable,size_,32) P(ZoneSetTable,buckets_,56)
S(ZoneSet,64) S(ZoneSetTable,64)
typedef decltype(((PlayerInterface*)0)->selectedCharacters) HandSet;
typedef boost::unordered::detail::set<HandSet::allocator_type,HandSet::value_type,HandSet::hasher,HandSet::key_equal>::table HandSetTable;
S(HandSet,64) S(HandSetTable,64)
typedef decltype(((GameWorld*)0)->charUpdateListMain) CharacterSet;
typedef boost::unordered::detail::set<CharacterSet::allocator_type,CharacterSet::value_type,CharacterSet::hasher,CharacterSet::key_equal>::table CharacterSetTable;
S(CharacterSet,64) S(CharacterSetTable,64)
typedef decltype(((GameWorld*)0)->destroyListOE) MovableSet;
typedef boost::unordered::detail::set<MovableSet::allocator_type,MovableSet::value_type,MovableSet::hasher,MovableSet::key_equal>::table MovableSetTable;
S(MovableSet,64) S(MovableSetTable,64)
typedef decltype(((GameWorld*)0)->killListPhase0) RootSet;
typedef boost::unordered::detail::set<RootSet::allocator_type,RootSet::value_type,RootSet::hasher,RootSet::key_equal>::table RootSetTable;
S(RootSet,64) S(RootSetTable,64)
typedef decltype(((GameWorld*)0)->deathParade) DeathMap;
typedef boost::unordered::detail::map<DeathMap::allocator_type,DeathMap::key_type,DeathMap::mapped_type,DeathMap::hasher,DeathMap::key_equal>::table DeathMapTable;
S(DeathMap,64) S(DeathMapTable,64)
typedef decltype(((Blackboard*)0)->requests) RequestMap;
typedef boost::unordered::detail::map<RequestMap::allocator_type,RequestMap::key_type,RequestMap::mapped_type,RequestMap::hasher,RequestMap::key_equal>::table RequestMapTable;
S(RequestMap,64) S(RequestMapTable,64)
typedef boost::unordered::detail::ptr_node<ZoneMap*> ZoneSetNode;
typedef boost::unordered::detail::ptr_node<hand> HandSetNode;
P(ZoneSetNode,next_,0) P(ZoneSetNode,value_base_,16)
P(HandSetNode,next_,0) P(HandSetNode,value_base_,16)


typedef decltype(((NavMesh*)0)->sectors) SectorMap;
typedef std::deque<Tasker*> TaskDeque;
P(SectorMap,_Myhead,8) P(SectorMap,_Mysize,16) P(TaskDeque,_Mysize,32)
S(SectorMap,40) S(TaskDeque,48)

typedef SectorMap::_Node SectorNode;
typedef SectorMap::value_type SectorValue;
P(SectorValue,first,0) P(SectorValue,second,8)
static_assert(sizeof(((ZoneMap*)0)->stateT.mainThreadData)==2,"promotion WORD");
static_assert(sizeof(((ZoneMap*)0)->stateT.mainThreadData._zoneBeingLoaded)==1,"loading BYTE");
static_assert(sizeof(((ZoneMap*)0)->stateT.mainThreadData._zoneIsLoaded)==1,"loaded BYTE");
static_assert(sizeof(((NavMeshGenerator::TaskQueue*)0)->back)==8,"tail pointer-to-pointer");

// HavokCharacter.h declares result nodes completely; they are not opaque.
P(EdgePathNode,mLeft,0) P(EdgePathNode,mRight,0x10)
P(EdgePathNode,face,0x20) P(EdgePathNode,edge,0x24)
P(EdgePathNode,leftClearance,0x28) P(EdgePathNode,rightClearance,0x2C)
P(EdgePathNode,maxPoint,0x30) S(EdgePathNode,64)
static_assert(__alignof(EdgePathNode)==16,"result node alignment");
static_assert(sizeof(((EdgePathNode*)0)->mLeft)==16,"left endpoint width");
static_assert(sizeof(((EdgePathNode*)0)->mRight)==16,"right endpoint width");
static_assert(sizeof(((EdgePathNode*)0)->face)==4,"node face width");
static_assert(sizeof(((EdgePathNode*)0)->edge)==4,"node edge width");
typedef hkArray<EdgePathNode,hkContainerHeapAllocator> ResultPathArray;
typedef hkArray<unsigned int,hkContainerHeapAllocator> VisitedKeyArray;
P(ResultPathArray,m_data,0) P(ResultPathArray,m_size,8) P(ResultPathArray,m_capacityAndFlags,12)
P(VisitedKeyArray,m_data,0) P(VisitedKeyArray,m_size,8) P(VisitedKeyArray,m_capacityAndFlags,12)
S(ResultPathArray,16) S(VisitedKeyArray,16)
static_assert(sizeof(((ResultPathArray*)0)->m_size)==4,"result count read/restore width");
static_assert(sizeof(*((ResultPathArray*)0)->m_data)==64,"result-node stride");
static_assert(sizeof(*((VisitedKeyArray*)0)->m_data)==4,"visited-key stride");
P(Renderer,workspace,0x88)
P(Renderer,camera,0x58)

template<class Map> struct SectorNodeProof
{
 typedef typename Map::_Node Node;
 typedef typename Map::value_type Value;
 static_assert(offsetof(Node,_Myval)==KLIB_OFF_SectorMapNode_value,"node pair offset");
 static_assert(offsetof(Value,second)==8,"mapped value offset");
 static_assert(offsetof(Node,_Myval)+offsetof(Value,second)==0x20,"legacy node mapped pointer offset");
 static_assert(sizeof(((Node*)0)->_Myval)==16,"node pair width");
 static_assert(sizeof(((Value*)0)->second)==8,"mapped pointer width");
 static_assert(sizeof(Node)==48,"map node stride");
};
static_assert(sizeof(SectorNodeProof<SectorMap>)==1,"instantiate node layout proof");


typedef lektor<ZoneMap*> ZoneLektor;
typedef lektor<Character*> CharacterLektor;
typedef lektor<RootObject*> RootLektor;
typedef lektor<NavInstance*> InstanceLektor;
typedef lektor<Tasker*> TaskLektor;
typedef lektor<Faction*> FactionLektor;
typedef lektor<Platoon*> PlatoonLektor;
typedef hkArrayBase<unsigned char> ByteArray;
typedef lektor<Compositor> CompositorLektor;
typedef boost::unordered::detail::ptr_node<Character*> CharacterSetNode;
typedef boost::unordered::detail::value_base<hand> HandNodeStorage;
typedef boost::unordered::detail::value_base<ZoneMap*> ZoneNodeStorage;
P(HandNodeStorage,data_,0) P(ZoneNodeStorage,data_,0)
static_assert(sizeof(((HandNodeStorage*)0)->data_)==32,"handle node value width");
static_assert(sizeof(((ZoneNodeStorage*)0)->data_)==8,"zone node pointer width");
#define KLIB_FIELD(N,T,M,O,W) static_assert(offsetof(T,M)==O, #N " offset"); static_assert(sizeof(((T*)0)->M)==W, #N " width");
#include "game/klib_member_fields.inc"
#undef KLIB_FIELD
// VS2010 offsetof rejects an indexed member; prove the union and element stride.
static_assert(offsetof(hkVector4f,m_quad)==0,"SIMD member offset");
#if defined(__clang__) || defined(__GNUC__)
// Here __m128 is a builtin vector with no named members, so the float lanes
// come from an overlay; its width must match the vector's, or the lane
// offsets below would be measured against a padded stand-in.
namespace klib_layout_detail { union KlibQuad { __m128 quad; float m128_f32[4]; }; }
using namespace klib_layout_detail;
static_assert(sizeof(KlibQuad)==sizeof(__m128),"SIMD overlay width");
static_assert(offsetof(KlibQuad,m128_f32)==0,"SIMD float union offset");
static_assert(sizeof(((KlibQuad*)0)->m128_f32)==16,"SIMD float array width");
static_assert(sizeof(((KlibQuad*)0)->m128_f32[0])==4,"SIMD scalar read width");
static_assert(KLIB_OFF_hkVector4f_x==offsetof(hkVector4f,m_quad)+offsetof(KlibQuad,m128_f32)+0*sizeof(float),"SIMD x component offset");
static_assert(KLIB_OFF_hkVector4f_y==offsetof(hkVector4f,m_quad)+offsetof(KlibQuad,m128_f32)+1*sizeof(float),"SIMD y component offset");
static_assert(KLIB_OFF_hkVector4f_z==offsetof(hkVector4f,m_quad)+offsetof(KlibQuad,m128_f32)+2*sizeof(float),"SIMD z component offset");
static_assert(KLIB_OFF_hkVector4f_w==offsetof(hkVector4f,m_quad)+offsetof(KlibQuad,m128_f32)+3*sizeof(float),"SIMD w component offset");
#else
static_assert(offsetof(__m128,m128_f32)==0,"SIMD float union offset");
static_assert(sizeof(((__m128*)0)->m128_f32)==16,"SIMD float array width");
static_assert(sizeof(((__m128*)0)->m128_f32[0])==4,"SIMD scalar read width");
static_assert(KLIB_OFF_hkVector4f_x==offsetof(hkVector4f,m_quad)+offsetof(__m128,m128_f32)+0*sizeof(float),"SIMD x component offset");
static_assert(KLIB_OFF_hkVector4f_y==offsetof(hkVector4f,m_quad)+offsetof(__m128,m128_f32)+1*sizeof(float),"SIMD y component offset");
static_assert(KLIB_OFF_hkVector4f_z==offsetof(hkVector4f,m_quad)+offsetof(__m128,m128_f32)+2*sizeof(float),"SIMD z component offset");
static_assert(KLIB_OFF_hkVector4f_w==offsetof(hkVector4f,m_quad)+offsetof(__m128,m128_f32)+3*sizeof(float),"SIMD w component offset");
#endif
#include "game/klib_members.h"
#define KLIB_FIELD(N,T,M,O,W) uintptr_t KlibField_##N(uintptr_t base) { return (uintptr_t)&((T*)base)->M; }
#include "game/klib_member_fields.inc"
#undef KLIB_FIELD
#if defined(__clang__) || defined(__GNUC__)
uintptr_t KlibField_hkVector4f_x(uintptr_t base) { return (uintptr_t)&((KlibQuad*)&((hkVector4f*)base)->m_quad)->m128_f32[0]; }
uintptr_t KlibField_hkVector4f_y(uintptr_t base) { return (uintptr_t)&((KlibQuad*)&((hkVector4f*)base)->m_quad)->m128_f32[1]; }
uintptr_t KlibField_hkVector4f_z(uintptr_t base) { return (uintptr_t)&((KlibQuad*)&((hkVector4f*)base)->m_quad)->m128_f32[2]; }
uintptr_t KlibField_hkVector4f_w(uintptr_t base) { return (uintptr_t)&((KlibQuad*)&((hkVector4f*)base)->m_quad)->m128_f32[3]; }
#else
uintptr_t KlibField_hkVector4f_x(uintptr_t base) { return (uintptr_t)&((hkVector4f*)base)->m_quad.m128_f32[0]; }
uintptr_t KlibField_hkVector4f_y(uintptr_t base) { return (uintptr_t)&((hkVector4f*)base)->m_quad.m128_f32[1]; }
uintptr_t KlibField_hkVector4f_z(uintptr_t base) { return (uintptr_t)&((hkVector4f*)base)->m_quad.m128_f32[2]; }
uintptr_t KlibField_hkVector4f_w(uintptr_t base) { return (uintptr_t)&((hkVector4f*)base)->m_quad.m128_f32[3]; }
#endif
uintptr_t KlibField_ZoneMapContent_items_size(uintptr_t base) { return KlibField_HandSetTable_size_(KlibField_ZoneMapContent_items(base)); }
uintptr_t KlibField_PlayerInterface_selected_count(uintptr_t base) { return KlibField_HandSetTable_size_(KlibField_PlayerInterface_selectedCharacters(base)); }
uintptr_t KlibField_PlayerInterface_selected_buckets(uintptr_t base) { return KlibField_HandSetTable_buckets_(KlibField_PlayerInterface_selectedCharacters(base)); }
uintptr_t KlibField_PlayerInterface_selected_bucketCount(uintptr_t base) { return KlibField_HandSetTable_bucket_count_(KlibField_PlayerInterface_selectedCharacters(base)); }
uintptr_t KlibField_HandSetNode_handle_type(uintptr_t base) { return KlibField_hand_type(KlibField_HandSetNode_value_base_(base)); }
uintptr_t KlibField_Blackboard_requests_size(uintptr_t base) { return KlibField_RequestMapTable_size_(KlibField_Blackboard_requests(base)); }
template<class Map> uintptr_t SectorMappedAddress(uintptr_t base)
{
 typedef typename Map::_Node Node;
 return (uintptr_t)&((Node*)base)->_Myval.second;
}
uintptr_t KlibField_SectorNode_mapped(uintptr_t base) { return SectorMappedAddress<SectorMap>(base); }
uintptr_t KlibField_GameWorld_deathParade_size(uintptr_t base) { return KlibField_DeathMapTable_size_(KlibField_GameWorld_deathParade(base)); }
uintptr_t KlibField_GameWorld_charUpdateListMain_size(uintptr_t base) { return KlibField_CharacterSetTable_size_(KlibField_GameWorld_charUpdateListMain(base)); }
uintptr_t KlibField_ZoneManager_activeZones_size(uintptr_t base) { return KlibField_ZoneSetTable_size_(KlibField_ZoneManager_activeZones(base)); }
uintptr_t KlibCompositorEntry(uintptr_t base, size_t index) { return (uintptr_t)&((Compositor*)base)[index]; }
uintptr_t KlibZoneEntry(uintptr_t base, size_t index) { return (uintptr_t)&((ZoneManager*)base)->worldMap[index / 64][index % 64]; }
uintptr_t KlibZoneNeighbor(uintptr_t base, size_t index) { return (uintptr_t)&((ZoneMap*)base)->neighbors[index]; }
static_assert(sizeof(((ZoneManager*)0)->worldMap) / sizeof(((ZoneManager*)0)->worldMap[0]) == KLIB_ZONE_DIMENSION, "worldMap rows");
static_assert(sizeof(((ZoneManager*)0)->worldMap[0]) / sizeof(((ZoneManager*)0)->worldMap[0][0]) == KLIB_ZONE_DIMENSION, "worldMap columns");
static_assert(sizeof(((ZoneManager*)0)->worldMap[0]) == KLIB_ZONE_DIMENSION * KLIB_ZONE_STRIDE, "worldMap row stride");
#include "base/klib_include_end.h"
