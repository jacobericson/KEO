#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/fog_fade.h"
#include "game/game.h"
#include "base/core.h"

// WaterOcclusionListener::frameRenderingQueued runs on the main thread (Ogre's
// render loop) once per frame, after the compositor has rendered that frame, so the byte it
// leaves in the Water_Reflection node's m_enabled decides whether the next
// frame renders the reflection. `self` is the FrameListener sub-object (the
// listener's second base, object+8): self+24 is the node, self+16 the
// occlusion query.
//
// A skipped reflection is one render_scene pass fewer, so the fog fades lose
// one update that frame. The game writes the frame dt in frameEnded, after
// this listener, so the dt the skipped frame used is still current at the
// next call of this hook; the missing update is made there.
typedef char (*OcclusionListener_t)(void* self, void* frameEvent);
typedef void* (*RootSingleton_t)();
typedef unsigned long (*RootFrameNumber_t)(void* root);

static const size_t NODE_OFFSET    = 24;
static const size_t ENABLED_OFFSET = 16;   // Ogre::CompositorNode::m_enabled

static OcclusionListener_t s_orig          = NULL;
static RootSingleton_t     s_rootSingleton = NULL;
static RootFrameNumber_t   s_rootFrame     = NULL;
static uintptr_t           s_node          = 0;
static unsigned char       s_gameDecision  = 0;   // the byte the game last left in the node
static uintptr_t           s_fogPending    = 0;   // the node whose reflection this hook switched off

static unsigned long FrameNumber()
{
	void* root = s_rootSingleton();
	return root ? s_rootFrame(root) : 0;
}

// The original writes the byte only when a query result arrives (or 0 when
// reflections are off); otherwise it leaves it untouched. Restoring the
// game's own decision first means the byte read after the call is always
// the game's, never the parity applied on the previous frame.
static char hook_OcclusionUpdate(void* self, void* frameEvent)
{
	unsigned char* enabled = NULL;
	uintptr_t node = *(uintptr_t*)((char*)self + NODE_OFFSET);
	if (s_fogPending)
	{
		if (s_fogPending == node && FogFadeCompensate(1))
			InterlockedIncrement(&g_renderStats.reflFog);
		s_fogPending = 0;
	}
	if (node)
	{
		enabled = (unsigned char*)(node + ENABLED_OFFSET);
		if (node != s_node)
		{
			s_node = node;
			s_gameDecision = *enabled;
		}
		*enabled = s_gameDecision;
	}
	char r = s_orig(self, frameEvent);
	if (enabled)
	{
		s_gameDecision = *enabled;
		if (s_gameDecision && g_renderCfg.reflectionHalfRate && (FrameNumber() & 1))
		{
			*enabled = 0;
			s_fogPending = node;
			InterlockedIncrement(&g_renderStats.reflSkipped);
		}
		else if (s_gameDecision)
			InterlockedIncrement(&g_renderStats.reflKept);
	}
	return r;
}

bool InstallReflectionLever()
{
	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	if (!ogre)
		return false;
	s_rootSingleton = (RootSingleton_t)GetProcAddress(ogre, "?getSingletonPtr@Root@Ogre@@SAPEAV12@XZ");
	s_rootFrame     = (RootFrameNumber_t)GetProcAddress(ogre, "?getNextFrameNumber@Root@Ogre@@QEBAKXZ");
	if (!s_rootSingleton || !s_rootFrame)
		return false;
	if (!VerifyPrologueByRva(RVA_WATER_OCCLUSION_LISTENER))
		return false;
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(
		GameAddr(RVA_WATER_OCCLUSION_LISTENER), hook_OcclusionUpdate, &s_orig))
		return false;
	if (!InstallFogFade())
		LogMsg("Render: reflectionHalfRate: fog fade check failed, fades over water run slightly slower");
	return true;
}
