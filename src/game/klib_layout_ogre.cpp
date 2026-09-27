#include "game/klib_member_contract.h"
#include "base/klib_include.h"
#include <Windows.h>
#include <stddef.h>
#include <stdint.h>
#include <ogre/OgreRoot.h>
#include <ogre/OgreSharedPtr.h>
#include <ogre/OgreFastArray.h>
#include <ogre/OgreGpuProgramParams.h>
namespace Ogre { class Mesh; }
class Effect;
struct MeshPointerLayoutProof : Ogre::SharedPtr<Ogre::Mesh>
{
 static void Check()
 {
  static_assert(offsetof(MeshPointerLayoutProof,pRep)==0, "mesh SharedPtr pointer");
  static_assert(sizeof(((MeshPointerLayoutProof*)0)->pRep)==8, "mesh pointer width");
 }
};
static_assert(sizeof(Ogre::SharedPtr<Ogre::Mesh>)==16,"mesh SharedPtr width");
// Private offsets (mData=0, mSize=8, mCapacity=16) are verified reproducibly
// by tools/kenshilib/test_private_layout.bat using the VS2010 compiler layout report.
static_assert(sizeof(Ogre::FastArray<Effect*>)==24,"active Effect array header width");
static_assert(sizeof(((const Ogre::FastArray<Effect*>*)0)->size())==8,"active Effect array count width");
struct RootLayoutProbe: Ogre::Root { static unsigned long ReadNextFrame(uintptr_t base) { return ((const RootLayoutProbe*)base)->mNextFrame; } static void Check() { static_assert(offsetof(RootLayoutProbe,mNextFrame)==KLIB_OFF_Root_nextFrame,"Ogre::Root::mNextFrame"); static_assert(sizeof(((RootLayoutProbe*)0)->mNextFrame)==4,"Ogre frame counter width"); static_assert(offsetof(RootLayoutProbe,mFrameListeners)==KLIB_OFF_Root_frameListeners,"Ogre::Root::mFrameListeners"); static_assert(offsetof(RootLayoutProbe,mRemovedFrameListeners)==KLIB_OFF_Root_removedFrameListeners,"Ogre::Root::mRemovedFrameListeners"); static_assert(sizeof(((RootLayoutProbe*)0)->mFrameListeners)==KLIB_SIZE_Root_listenerSet,"Ogre listener set width"); } };
struct GpuParamsLayoutProbe: Ogre::GpuProgramParameters { static void Check() { static_assert(offsetof(GpuParamsLayoutProbe,mNamedConstants)==KLIB_OFF_GpuProgramParameters_namedConstants,"Ogre::GpuProgramParameters::mNamedConstants"); } };
#include "game/klib_members.h"
unsigned long KlibRootNextFrame(uintptr_t base) { return RootLayoutProbe::ReadNextFrame(base); }
uintptr_t KlibMeshPointer(uintptr_t base) { return (uintptr_t)((const Ogre::SharedPtr<Ogre::Mesh>*)base)->getPointer(); }
size_t KlibEffectCount(uintptr_t base) { return ((const Ogre::FastArray<Effect*>*)base)->size(); }
uintptr_t KlibEffectData(uintptr_t base) { return (uintptr_t)((const Ogre::FastArray<Effect*>*)base)->begin(); }
#include "base/klib_include_end.h"
#include "base/klib_include.h"
#pragma warning(disable: 4251 4275)
#include <ogre/OgreMovableObject.h>
#include <ogre/OgreAnimationState.h>
#include <ogre/OgreNode.h>
struct MovableObjectLayoutProbe : Ogre::MovableObject
{
 static void Check() { static_assert(offsetof(MovableObjectLayoutProbe,mParentNode)==KLIB_OFF_MovableObject_parentNode,"Ogre::MovableObject::mParentNode"); }
};
struct AnimationStateSetLayoutProbe : Ogre::AnimationStateSet
{
 static void Check()
 {
  static_assert(offsetof(AnimationStateSetLayoutProbe,mDirtyFrameNumber)==KLIB_OFF_AnimationStateSet_dirtyFrameNumber,"Ogre::AnimationStateSet::mDirtyFrameNumber");
  static_assert(sizeof(((AnimationStateSetLayoutProbe*)0)->mDirtyFrameNumber)==4,"dirty frame width");
 }
};
struct NodeLayoutProbe : Ogre::Node
{
 static void Check()
 {
  static_assert(offsetof(NodeLayoutProbe,mTransform)==KLIB_OFF_Node_transform,"Ogre::Node::mTransform");
 }
};
static_assert(offsetof(Ogre::Transform,mIndex)==KLIB_OFF_Transform_index,"Ogre::Transform::mIndex");
static_assert(sizeof(((Ogre::Transform*)0)->mIndex)==1,"transform index width");
static_assert(offsetof(Ogre::Transform,mDerivedTransform)==KLIB_OFF_Transform_derivedTransform,"Ogre::Transform::mDerivedTransform");
#include "base/klib_include_end.h"
