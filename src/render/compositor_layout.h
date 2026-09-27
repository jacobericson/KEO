#pragma once
#include <windows.h>

// OgreMain fields the empty-pass lever reads beyond the exports it calls.
// VerifyCompositorLayout checks each against the code that uses it.
static const size_t WS_FINAL_TARGET    = 0xA0;   // CompositorWorkspace: RenderTarget* it renders into
static const size_t WS_TARGET_WIDTH    = 0xC8;   // uint32: target width its textures were last sized to
static const size_t WS_TARGET_HEIGHT   = 0xCC;   // uint32: target height, likewise
static const size_t RT_GET_WIDTH_SLOT  = 3;      // RenderTarget vtable: getWidth()
static const size_t RT_GET_HEIGHT_SLOT = 4;      // RenderTarget vtable: getHeight()
static const size_t SM_CULL_LIST       = 0x398;  // SceneManager: vector<ObjectMemoryManager*> render_scene culls
static const size_t OMM_QUEUES         = 0x08;   // ObjectMemoryManager: per-queue slot array, begin (+8: end)

// Read by the visibility test (pass_visibility.h); VerifyPassVisibilityLayout
// checks each against the code that uses it.
static const size_t PASS_VIEWPORT      = 0x18;   // CompositorPass: Viewport* it renders into
static const size_t PASS_CAMERA        = 0x40;   // CompositorPassScene: Camera* it culls with
static const size_t VP_ACT_WIDTH       = 0x30;   // Viewport: int32 width in pixels
static const size_t VP_ACT_HEIGHT      = 0x34;   // Viewport: int32 height in pixels
static const size_t CAM_AUTO_ASPECT    = 0x608;  // Camera: bool, aspect follows each pass's viewport
static const size_t CAM_FRUSTUM_PLANES = 0x1EC;  // Frustum: Plane[6] the cull reads
static const size_t CAM_GET_ASPECT_SLOT = 36;    // Camera vtable: getAspectRatio()
static const size_t CAM_GET_PLANES_SLOT = 54;    // Camera vtable: getFrustumPlanes(), refreshes the planes

extern const char* const SYM_WS_FIND_NODE;
extern const char* const SYM_NODE_SET_ENABLED;
extern const char* const SYM_NODE_GET_ENABLED;
extern const char* const SYM_WS_SET_LISTENER;
extern const char* const SYM_WS_GET_LISTENER;
extern const char* const SYM_WS_DEFINITION_NAME;
extern const char* const SYM_WS_SCENE_MANAGER;
extern const char* const SYM_NODE_GET_PASSES;
extern const char* const SYM_PASS_SCENE_VTABLE;

// True when OgreMain's code reads the fields above where they are declared:
// CompositorWorkspace::_update (listener call, target size test),
// SceneManager::_cullPhase01 (cull list) and
// ObjectMemoryManager::getNumRenderQueues (per-queue slots).
bool VerifyCompositorLayout(HMODULE ogre);

// True when OgreMain's code reads the visibility test's fields where they are
// declared: SceneManager::_cullPhase01 (planes refreshed through the cull
// camera's vtable), SceneManager::cullFrustum (the queue slot's pools and
// object count), MovableObject::cullFrustum (planes, ObjectData pools and
// flag masks), CompositorPassScene::execute (pass camera and viewport),
// Viewport::_updateCullPhase01 (the automatic aspect) and
// Frustum::getFrustumPlanes.
bool VerifyPassVisibilityLayout(HMODULE ogre);
