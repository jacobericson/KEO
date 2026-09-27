#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/compositor_layout.h"
#include "render/empty_pass_policy.h"
#include "render/empty_pass_visibility.h"
#include "render/id_string.h"
#include "render/fog_fade.h"
#include "game/game.h"
#include "base/core.h"

// Switches two nodes of the Kenshi_Main workspace off while their passes
// would draw nothing: Debug (queues 87-88: debug lines, arrows stuck in
// characters, the placement ghost) and InteriorMask (queue 16: the mask boxes
// of loaded building interiors). A node goes off when its queues are empty,
// or when none of their objects can pass its cull: hidden, or outside the
// pass camera's frustum (empty_pass_visibility.h). A disabled node's passes
// are skipped whole, cull and render.
//
// The decision is made in a CompositorWorkspaceListener, whose first slot
// CompositorWorkspace::_update calls on the main thread before any node runs,
// so an object created anywhere earlier in the frame turns its node back on
// for that same frame. setEnabled writes only CompositorNode::mEnabled, which
// _update tests per node; connections and passes are untouched, so a node
// switched back on runs exactly as before. The main-thread tick attaches the
// listener to the current workspace (a new game recreates it) and, when the
// key goes off, restores the nodes and detaches it.
//
// Each skipped render_scene pass would have advanced the fog fades once; the
// listener makes up those updates (fog_fade.h). Without that call verified,
// the lever does not install.

typedef void* (*FindNode_t)(const void* ws, unsigned int name, bool includeShadowNodes);
typedef void  (*SetEnabled_t)(void* node, bool enabled);
typedef bool  (*GetEnabled_t)(const void* node);
typedef void  (*SetListener_t)(void* ws, void* listener);
typedef void* (*GetListener_t)(const void* ws);
// IdString is returned through a hidden pointer (rdx).
typedef unsigned int* (*GetDefinitionName_t)(const void* ws, unsigned int* out);
typedef void* (*GetSceneManager_t)(const void* ws);
typedef unsigned int (*TargetDim_t)(const void* target);

static const char* const OGRE_MODULE = "OgreMain_x64.dll";

static const size_t RENDERER_WORKSPACE = 0x88;   // Renderer::workspace

// The render_scene passes each node holds, from the compositor scripts.
static const LONG DEBUG_SCENE_PASSES = 1;
static const LONG MASK_SCENE_PASSES  = 2;

// The nodes' render_scene ranges [first, end), from the compositor scripts.
static const size_t DEBUG_QUEUE_FIRST = 87;
static const size_t DEBUG_QUEUE_END   = 89;
static const size_t MASK_QUEUE_FIRST  = 16;
static const size_t MASK_QUEUE_END    = 17;

static FindNode_t          s_findNode    = NULL;
static SetEnabled_t        s_setEnabled  = NULL;
static GetEnabled_t        s_getEnabled  = NULL;
static SetListener_t       s_setListener = NULL;
static GetListener_t       s_getListener = NULL;
static GetDefinitionName_t s_getDefName  = NULL;
static GetSceneManager_t   s_getScene    = NULL;
static bool                s_installed   = false;

static unsigned int s_hashMain  = 0;
static unsigned int s_hashDebug = 0;
static unsigned int s_hashMask  = 0;

// Main thread only (tick and listener). s_debugOff/s_maskOff: this lever
// switched the node off and it is still off. A node found under a new
// address is a new node, which starts enabled.
static void* s_ws           = NULL;   // the workspace carrying the listener
static void* s_debugNode    = NULL;
static void* s_maskNode     = NULL;
static bool  s_debugOff     = false;
static bool  s_maskOff      = false;
static bool  s_maskRanEmpty = false;
static bool  s_busy         = false;  // another listener holds the workspace

// Stats for the current window.
static LONG s_frames      = 0;
static LONG s_debugFrames = 0;   // frames the Debug node was off
static LONG s_maskFrames  = 0;   // frames the InteriorMask node was off
static LONG s_switches    = 0;   // setEnabled writes
static LONG s_fogCalls    = 0;   // fog updates made for skipped passes
static LONG s_debugVis    = 0;   // frames Debug stayed on for an object that may draw
static LONG s_maskVis     = 0;   // frames InteriorMask stayed on for one
static LONG s_unsure      = 0;   // frames a node stayed on because the test could not tell
static LONG s_debugScanMax = 0;  // most objects tested for Debug in one frame
static LONG s_maskScanMax  = 0;  // most objects tested for InteriorMask in one frame

// The listener as Ogre calls it: slot 0 workspacePreUpdate(workspace),
// slot 1 passPreExecute(pass); the spare slots are never called.
typedef void (*ListenerSlot_t)(void* self, void* arg);
struct ListenerObject { const ListenerSlot_t* vtbl; };
static void ListenerPreUpdate(void* self, void* ws);
static void ListenerIgnore(void*, void*) {}
static const ListenerSlot_t s_listenerVtbl[] =
	{ &ListenerPreUpdate, &ListenerIgnore, &ListenerIgnore, &ListenerIgnore };
static ListenerObject s_listener = { s_listenerVtbl };

// Whether the node's passes may draw this frame; counts why it stays on and
// the objects the test went through.
static bool PassDrawing(PassContent content, size_t scanned, LONG* visFrames, LONG* scanMax, bool* unsure)
{
	if ((LONG)scanned > *scanMax)
		*scanMax = (LONG)scanned;
	if (content == PASS_OCCUPIED)
		return true;
	if (content == PASS_MAY_DRAW)
	{
		++*visFrames;
		return true;
	}
	if (content == PASS_UNSURE)
	{
		*unsure = true;
		return true;
	}
	return false;
}

// True when this frame's _update will recreate the target-sized textures.
static bool ResizePending(void* ws)
{
	void* target = *(void**)((char*)ws + WS_FINAL_TARGET);
	if (!target)
		return true;
	void** vt = *(void***)target;
	unsigned int w = ((TargetDim_t)vt[RT_GET_WIDTH_SLOT])(target);
	unsigned int h = ((TargetDim_t)vt[RT_GET_HEIGHT_SLOT])(target);
	return w != *(const unsigned int*)((char*)ws + WS_TARGET_WIDTH) ||
	       h != *(const unsigned int*)((char*)ws + WS_TARGET_HEIGHT);
}

// Switches a node this lever manages. One this lever did not switch off but
// finds off belongs to someone else and is left alone; one it switched off
// but finds on was switched back by someone else and is managed afresh.
static void ApplyNode(void* node, bool wantOn, bool* weOff)
{
	bool enabled = s_getEnabled(node);
	if (*weOff && enabled)
		*weOff = false;
	if (!*weOff && !enabled)
		return;
	if (enabled == wantOn)
		return;
	s_setEnabled(node, wantOn);
	*weOff = !wantOn;
	++s_switches;
}

static void RestoreNode(void* node, bool* weOff)
{
	if (node && *weOff && !s_getEnabled(node))
	{
		s_setEnabled(node, true);
		++s_switches;
	}
	*weOff = false;
}

// Looks both nodes up again; a node under a new address starts clean.
static void RefreshNodes(void* ws)
{
	void* debugNode = s_findNode(ws, s_hashDebug, false);
	void* maskNode = s_findNode(ws, s_hashMask, false);
	if (debugNode != s_debugNode)
	{
		s_debugNode = debugNode;
		s_debugOff = false;
	}
	if (maskNode != s_maskNode)
	{
		s_maskNode = maskNode;
		s_maskOff = false;
		s_maskRanEmpty = false;
	}
}

static void RestoreNodes(void* ws)
{
	RefreshNodes(ws);
	RestoreNode(s_debugNode, &s_debugOff);
	RestoreNode(s_maskNode, &s_maskOff);
	s_maskRanEmpty = false;
}

static uintptr_t CurrentRenderer()
{
	uintptr_t holder = (uintptr_t)GameAddr(RVA_RENDERER);
	return holder ? *(const uintptr_t*)holder : 0;
}

// Main thread, inside CompositorWorkspace::_update before any node runs:
// no allocation, lock or logging.
static void ListenerPreUpdate(void*, void* ws)
{
	if (ws != s_ws)
		return;
	if (!g_renderCfg.emptyPassSkip)
	{
		RestoreNodes(ws);
		return;
	}
	RefreshNodes(ws);
	void* scene = s_getScene(ws);
	bool keepAll = !scene || !FogFadeAvailable();
	bool resize = ResizePending(ws);
	bool unsure = false;
	if (s_debugNode)
	{
		bool drawing = true;
		if (!keepAll)
		{
			size_t scanned = 0;
			PassContent content = NodePassContent(s_debugNode, scene, DEBUG_QUEUE_FIRST, DEBUG_QUEUE_END, resize, &scanned);
			drawing = PassDrawing(content, scanned, &s_debugVis, &s_debugScanMax, &unsure);
		}
		ApplyNode(s_debugNode, drawing, &s_debugOff);
	}
	if (s_maskNode)
	{
		bool enabled = s_getEnabled(s_maskNode);
		// Switched by someone else, on or off: its texture is not known clear.
		if ((enabled && s_maskOff) || (!enabled && !s_maskOff))
			s_maskRanEmpty = false;
		bool drawing = true;
		if (!keepAll)
		{
			size_t scanned = 0;
			PassContent content = NodePassContent(s_maskNode, scene, MASK_QUEUE_FIRST, MASK_QUEUE_END, resize, &scanned);
			drawing = PassDrawing(content, scanned, &s_maskVis, &s_maskScanMax, &unsure);
		}
		bool wantOn = MaskNodeWanted(drawing, resize, enabled, &s_maskRanEmpty);
		ApplyNode(s_maskNode, wantOn, &s_maskOff);
	}
	if (unsure)
		++s_unsure;
	LONG skipped = (s_debugOff ? DEBUG_SCENE_PASSES : 0) + (s_maskOff ? MASK_SCENE_PASSES : 0);
	if (skipped > 0 && FogFadeCompensate(skipped))
		s_fogCalls += skipped;
	++s_frames;
	if (s_debugOff)
		++s_debugFrames;
	if (s_maskOff)
		++s_maskFrames;
}

static void* CurrentWorkspace()
{
	uintptr_t renderer = CurrentRenderer();
	return renderer ? *(void**)(renderer + RENDERER_WORKSPACE) : NULL;
}

static void ForgetWorkspace()
{
	s_ws = NULL;
	s_debugNode = NULL;
	s_maskNode = NULL;
	s_debugOff = false;
	s_maskOff = false;
	s_maskRanEmpty = false;
}

static void Attach(void* ws)
{
	unsigned int name = 0;
	s_getDefName(ws, &name);
	if (name != s_hashMain)
		return;
	if (s_getListener(ws))
	{
		if (!s_busy)
			LogMsg("Render: emptyPassSkip: the main workspace already has a listener, not attached");
		s_busy = true;
		return;
	}
	s_busy = false;
	ForgetWorkspace();
	s_ws = ws;
	s_setListener(ws, &s_listener);
	RefreshNodes(ws);
	std::ostringstream ss;
	ss << "Render: emptyPassSkip: attached to Kenshi_Main (Debug " << (s_debugNode ? "found" : "missing")
	   << ", InteriorMask " << (s_maskNode ? "found" : "missing") << ")";
	LogMsg(ss.str());
}

void EmptyPass_MainThreadTick()
{
	if (!s_installed)
		return;
	void* ws = CurrentWorkspace();
	// A replaced workspace was freed with its nodes; its successor starts
	// with every node enabled and no listener.
	if (s_ws && ws != s_ws)
		ForgetWorkspace();
	if (s_ws && s_getListener(s_ws) != (void*)&s_listener)
	{
		RestoreNodes(s_ws);
		ForgetWorkspace();
	}
	if (!g_renderCfg.emptyPassSkip)
	{
		if (s_ws)
		{
			RestoreNodes(s_ws);
			s_setListener(s_ws, NULL);
			ForgetWorkspace();
		}
		s_busy = false;
		return;
	}
	if (!s_ws && ws)
		Attach(ws);
}

std::string EmptyPassStatsToken()
{
	if (!s_installed)
		return std::string();
	LONG frames = s_frames, debugFrames = s_debugFrames, maskFrames = s_maskFrames, switches = s_switches;
	LONG fogCalls = s_fogCalls, debugVis = s_debugVis, maskVis = s_maskVis, unsure = s_unsure;
	LONG debugScan = s_debugScanMax, maskScan = s_maskScanMax;
	s_frames = s_debugFrames = s_maskFrames = s_switches = s_fogCalls = 0;
	s_debugVis = s_maskVis = s_unsure = s_debugScanMax = s_maskScanMax = 0;
	if (!g_renderCfg.emptyPassSkip)
		return std::string();
	if (!s_ws)
		return s_busy ? " emptyPass=busy" : " emptyPass=noWorkspace";
	std::ostringstream ss;
	ss << " debugOff=" << (s_debugOff ? 1 : 0) << " maskOff=" << (s_maskOff ? 1 : 0)
	   << " passOffFrames=" << debugFrames << "/" << maskFrames << "/" << frames
	   << " passSw=" << switches << " fogComp=" << fogCalls;
	if (PassVisibilityAvailable())
		ss << " passVis=" << debugVis << "/" << maskVis << " visUnsure=" << unsure
		   << " visScanMax=" << debugScan << "/" << maskScan;
	else
		ss << " passVis=off";
	return ss.str();
}

static bool Resolve(HMODULE ogre, const char* symbol, void** out)
{
	*out = (void*)GetProcAddress(ogre, symbol);
	if (*out)
		return true;
	LogMsg(std::string("Render: emptyPassSkip: OgreMain export missing: ") + symbol);
	return false;
}

bool InstallEmptyPassSkip()
{
	HMODULE ogre = GetModuleHandleA(OGRE_MODULE);
	if (!ogre)
	{
		LogMsg("Render: emptyPassSkip: OgreMain not loaded");
		return false;
	}
	if (!Resolve(ogre, SYM_WS_FIND_NODE, (void**)&s_findNode) ||
	    !Resolve(ogre, SYM_NODE_SET_ENABLED, (void**)&s_setEnabled) ||
	    !Resolve(ogre, SYM_NODE_GET_ENABLED, (void**)&s_getEnabled) ||
	    !Resolve(ogre, SYM_WS_SET_LISTENER, (void**)&s_setListener) ||
	    !Resolve(ogre, SYM_WS_GET_LISTENER, (void**)&s_getListener) ||
	    !Resolve(ogre, SYM_WS_DEFINITION_NAME, (void**)&s_getDefName) ||
	    !Resolve(ogre, SYM_WS_SCENE_MANAGER, (void**)&s_getScene))
		return false;
	if (!VerifyCompositorLayout(ogre))
	{
		LogMsg("Render: emptyPassSkip: OgreMain compositor layout not as expected");
		return false;
	}
	// Skipped passes must keep fog fades at speed, so the lever installs only
	// when the call it repeats is verified.
	if (!InstallFogFade())
	{
		LogMsg("Render: emptyPassSkip: the fog fade call could not be verified, lever off");
		return false;
	}
	if (!InstallPassVisibility(ogre))
		LogMsg("Render: emptyPassSkip: OgreMain cull layout not as expected, nodes go off only when their queues are empty");
	s_hashMain = IdStringHash("Kenshi_Main");
	s_hashDebug = IdStringHash("Debug");
	s_hashMask = IdStringHash("InteriorMask");
	s_installed = true;
	return true;
}
