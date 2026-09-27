#include "render/compositor_layout.h"
#include "render/empty_pass_policy.h"
#include "render/pass_visibility.h"
#include <cstring>

// No game calls, so the host tests check it against the shipped OgreMain.

const char* const SYM_WS_FIND_NODE       = "?findNode@CompositorWorkspace@Ogre@@QEBAPEAVCompositorNode@2@UIdString@2@_N@Z";
const char* const SYM_NODE_SET_ENABLED   = "?setEnabled@CompositorNode@Ogre@@QEAAX_N@Z";
const char* const SYM_NODE_GET_ENABLED   = "?getEnabled@CompositorNode@Ogre@@QEBA_NXZ";
const char* const SYM_WS_SET_LISTENER    = "?setListener@CompositorWorkspace@Ogre@@QEAAXPEAVCompositorWorkspaceListener@2@@Z";
const char* const SYM_WS_GET_LISTENER    = "?getListener@CompositorWorkspace@Ogre@@QEBAPEAVCompositorWorkspaceListener@2@XZ";
const char* const SYM_WS_DEFINITION_NAME = "?getDefinitionName@CompositorWorkspace@Ogre@@QEBA?BUIdString@2@XZ";
const char* const SYM_WS_SCENE_MANAGER   = "?getSceneManager@CompositorWorkspace@Ogre@@QEBAPEAVSceneManager@2@XZ";
const char* const SYM_NODE_GET_PASSES    = "?_getPasses@CompositorNode@Ogre@@QEBAAEBV?$vector@PEAVCompositorPass@Ogre@@V?$STLAllocator@PEAVCompositorPass@Ogre@@V?$CategorisedAllocPolicy@$0A@@2@@2@@std@@XZ";
const char* const SYM_PASS_SCENE_VTABLE  = "??_7CompositorPassScene@Ogre@@6B@";

static const char* const SYM_WS_UPDATE         = "?_update@CompositorWorkspace@Ogre@@QEAAXXZ";
static const char* const SYM_SM_CULL_PHASE01   = "?_cullPhase01@SceneManager@Ogre@@UEAAXPEAVCamera@2@PEBV32@PEAVViewport@2@EE@Z";
static const char* const SYM_OMM_NUM_QUEUES    = "?getNumRenderQueues@ObjectMemoryManager@Ogre@@QEBA_KXZ";
static const char* const SYM_SM_CULL_FRUSTUM   = "?cullFrustum@SceneManager@Ogre@@IEAAXAEBUCullFrustumRequest@2@_K@Z";
static const char* const SYM_MO_CULL_FRUSTUM   = "?cullFrustum@MovableObject@Ogre@@SAX_KUObjectData@2@PEBVFrustum@2@IAEAV?$FastArray@PEAVMovableObject@Ogre@@@2@PEBVCamera@2@@Z";
static const char* const SYM_PASS_SCENE_EXEC   = "?execute@CompositorPassScene@Ogre@@UEAAXPEBVCamera@2@@Z";
static const char* const SYM_VP_CULL_PHASE01   = "?_updateCullPhase01@Viewport@Ogre@@QEAAXPEAVCamera@2@PEBV32@EE@Z";
static const char* const SYM_FRUSTUM_PLANES    = "?getFrustumPlanes@Frustum@Ogre@@UEBAPEBVPlane@2@XZ";

// _update, first thing: mov rcx,[rcx+20h]; test rcx,rcx; jz; mov rax,[rcx];
// mov rdx,rsi; call [rax] -- the listener's slot 0 with the workspace.
static const size_t        UPDATE_LISTENER_AT = 0x37;
static const unsigned char UPDATE_LISTENER[17] =
	{ 0x48,0x8B,0x49,0x20, 0x48,0x85,0xC9, 0x74,0x08, 0x48,0x8B,0x01, 0x48,0x8B,0xD6, 0xFF,0x10 };
// Then the resize test: target [rsi+0A0h], vtable +18h (getWidth) against
// [rsi+0C8h], vtable +20h (getHeight) against [rsi+0CCh].
static const size_t        UPDATE_RESIZE_AT = 0x5F;
static const unsigned char UPDATE_RESIZE[43] =
{
	0x48,0x8B,0x8E,0xA0,0x00,0x00,0x00, 0x48,0x8B,0x01, 0xFF,0x50,0x18, 0x45,0x33,0xED,
	0x39,0x86,0xC8,0x00,0x00,0x00, 0x75,0x19,
	0x48,0x8B,0x8E,0xA0,0x00,0x00,0x00, 0x48,0x8B,0x01, 0xFF,0x50,0x20,
	0x39,0x86,0xCC,0x00,0x00,0x00
};
// _cullPhase01: lea r12,[rdi+398h]; mov rcx,[r12]; mov r8,[rdi+3A0h].
static const size_t        CULL_LIST_AT = 0xB3;
static const unsigned char CULL_LIST[18] =
	{ 0x4C,0x8D,0xA7,0x98,0x03,0x00,0x00, 0x49,0x8B,0x0C,0x24, 0x4C,0x8B,0x87,0xA0,0x03,0x00,0x00 };
// getNumRenderQueues: the slot array at +8/+10h, and per queue slot
// [+68h]-[+60h] freed, [+40h] used, 0A0h stride.
static const unsigned char QUEUES_ARRAY[8] = { 0x4C,0x8B,0x41,0x08, 0x48,0x8B,0x49,0x10 };
static const size_t        QUEUES_COUNT_AT = 0x30;
static const unsigned char QUEUES_COUNT[12] = { 0x49,0x8B,0x50,0x68, 0x49,0x8B,0x40,0x40, 0x49,0x2B,0x50,0x60 };
static const size_t        QUEUES_STRIDE_AT = 0x69;
static const unsigned char QUEUES_STRIDE[7] = { 0x49,0x81,0xC0,0xA0,0x00,0x00,0x00 };

static bool InImage(HMODULE module, const void* p, size_t len)
{
	if (!module || !p)
		return false;
	uintptr_t base = (uintptr_t)module;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	uintptr_t a = (uintptr_t)p;
	return a >= base && a + len <= base + nt->OptionalHeader.SizeOfImage;
}

static bool BytesAt(HMODULE module, const char* symbol, size_t at, const unsigned char* want, size_t len)
{
	const unsigned char* fn = (const unsigned char*)GetProcAddress(module, symbol);
	return fn && InImage(module, fn + at, len) && memcmp(fn + at, want, len) == 0;
}

bool VerifyCompositorLayout(HMODULE ogre)
{
	if (!ogre)
		return false;
	// The byte checks below spell these out; keep the two in step.
	if (WS_FINAL_TARGET != 0xA0 || WS_TARGET_WIDTH != 0xC8 || WS_TARGET_HEIGHT != 0xCC ||
	    RT_GET_WIDTH_SLOT * 8 != 0x18 || RT_GET_HEIGHT_SLOT * 8 != 0x20 || SM_CULL_LIST != 0x398 ||
	    OMM_QUEUES != 0x08 || QUEUE_SLOT_SIZE != 0xA0 || QUEUE_USED != 0x40 ||
	    QUEUE_FREE_BEGIN != 0x60 || QUEUE_FREE_END != 0x68)
		return false;
	return BytesAt(ogre, SYM_WS_UPDATE, UPDATE_LISTENER_AT, UPDATE_LISTENER, sizeof(UPDATE_LISTENER)) &&
	       BytesAt(ogre, SYM_WS_UPDATE, UPDATE_RESIZE_AT, UPDATE_RESIZE, sizeof(UPDATE_RESIZE)) &&
	       BytesAt(ogre, SYM_SM_CULL_PHASE01, CULL_LIST_AT, CULL_LIST, sizeof(CULL_LIST)) &&
	       BytesAt(ogre, SYM_OMM_NUM_QUEUES, 0, QUEUES_ARRAY, sizeof(QUEUES_ARRAY)) &&
	       BytesAt(ogre, SYM_OMM_NUM_QUEUES, QUEUES_COUNT_AT, QUEUES_COUNT, sizeof(QUEUES_COUNT)) &&
	       BytesAt(ogre, SYM_OMM_NUM_QUEUES, QUEUES_STRIDE_AT, QUEUES_STRIDE, sizeof(QUEUES_STRIDE));
}

// _cullPhase01: mov rcx,[rdi+4A40h] (the cull camera); mov rax,[rcx];
// call [rax+1B0h] -- getFrustumPlanes, just before the cull threads start.
static const size_t        CULL_PLANES_AT = 0x1E2;
static const unsigned char CULL_PLANES[16] =
	{ 0x48,0x8B,0x8F,0x40,0x4A,0x00,0x00, 0x48,0x8B,0x01, 0xFF,0x90,0xB0,0x01,0x00,0x00 };
// getFrustumPlanes: refresh through vtable +0A0h, return this+1ECh.
static const unsigned char GET_PLANES[25] =
{
	0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0x01, 0x48,0x8B,0xD9, 0xFF,0x90,0xA0,0x00,0x00,0x00,
	0x48,0x8D,0x83,0xEC,0x01,0x00,0x00
};
// SceneManager::cullFrustum, per queue slot: slots [r15+8]; pool vector
// [slot+8]/[slot+10h] copied to ObjectData+8 (the byte at +0 zeroed); object
// count [slot+40h].
static const size_t        SM_POOLS_AT = 0x115;
static const unsigned char SM_POOLS[14] =
	{ 0x49,0x8B,0x77,0x08, 0x49,0x8B,0x54,0x35,0x08, 0x4D,0x8B,0x44,0x35,0x10 };
static const size_t        SM_OBJDATA_AT = 0x123;
static const unsigned char SM_OBJDATA[13] =
	{ 0x48,0x8D,0x4C,0x24,0x48, 0x4C,0x2B,0xC2, 0xC6,0x44,0x24,0x40,0x00 };
static const size_t        SM_COUNT_AT = 0x139;
static const unsigned char SM_COUNT[5] = { 0x4D,0x8B,0x5C,0x35,0x40 };
// MovableObject::cullFrustum: mask &= 1FFFFFFFh; six planes read from
// frustum+1F4h-8 (x -8, y -4, z +0, d +4, 10h apart); ObjectData flags +40h
// and world AABB +20h; LAYER_VISIBILITY from a 16-byte constant.
static const size_t        MO_MASK_AT = 0x10C;
static const unsigned char MO_MASK[7] = { 0x41,0x81,0xE1,0xFF,0xFF,0xFF,0x1F };
static const size_t        MO_PLANES_AT = 0x12D;
static const unsigned char MO_PLANES[12] = { 0x49,0x81,0xC0,0xF4,0x01,0x00,0x00, 0xBA,0x06,0x00,0x00,0x00 };
static const size_t        MO_PLANE_X_AT = 0x150;
static const unsigned char MO_PLANE_X[6] = { 0xF3,0x41,0x0F,0x10,0x50,0xF8 };
static const size_t        MO_PLANE_Y_AT = 0x15E;
static const unsigned char MO_PLANE_Y[6] = { 0xF3,0x41,0x0F,0x10,0x48,0xFC };
static const size_t        MO_PLANE_Z_AT = 0x16C;
static const unsigned char MO_PLANE_Z[5] = { 0xF3,0x41,0x0F,0x10,0x00 };
static const size_t        MO_PLANE_D_AT = 0x1A7;
static const unsigned char MO_PLANE_D[6] = { 0xF3,0x41,0x0F,0x10,0x40,0x04 };
static const size_t        MO_PLANE_NEXT_AT = 0x1B8;
static const unsigned char MO_PLANE_NEXT[4] = { 0x49,0x83,0xC0,0x10 };
static const size_t        MO_POOLS_AT = 0x210;
static const unsigned char MO_POOLS[16] =
	{ 0x4C,0x8B,0x43,0x40, 0x48,0x8B,0x53,0x30, 0x48,0x8B,0x4B,0x38, 0x48,0x8B,0x43,0x20 };
static const size_t        MO_LAYER_AT = 0x4CD;   // pand xmm1,[rip+disp32]
static const unsigned char MO_LAYER[4] = { 0x66,0x0F,0xDB,0x0D };
// CompositorPassScene::execute: camera [r12+40h], viewport [r12+18h], then
// Viewport::_updateCullPhase01(viewport, camera, ...).
static const size_t        EXEC_CAMERA_AT = 0x25A;
static const unsigned char EXEC_CAMERA[10] = { 0x49,0x8B,0x74,0x24,0x40, 0x49,0x8B,0x6C,0x24,0x18 };
static const size_t        EXEC_CULL_AT = 0x280;
static const unsigned char EXEC_CULL[7] = { 0x48,0x8B,0xD6, 0x48,0x8B,0xCD, 0xE8 };
// Viewport::_updateCullPhase01: when camera [+608h] is set, compares
// getAspectRatio (vtable +120h) with width [+30h] / height [+34h].
static const size_t        VP_ASPECT_AT = 0x14;
static const unsigned char VP_ASPECT[12] = { 0x80,0xBA,0x08,0x06,0x00,0x00,0x00, 0x66,0x0F,0x6E,0x41,0x34 };
static const size_t        VP_WIDTH_AT = 0x32;
static const unsigned char VP_WIDTH[5] = { 0x66,0x0F,0x6E,0x71,0x30 };
static const size_t        VP_GET_ASPECT_AT = 0x49;
static const unsigned char VP_GET_ASPECT[6] = { 0xFF,0x90,0x20,0x01,0x00,0x00 };

// The 16-byte constant a rip-relative operand at `at` (prefix, then disp32)
// points to holds `value` in every lane.
static bool RipConstantIs(HMODULE module, const char* symbol, size_t at, size_t prefixLen, unsigned int value)
{
	const unsigned char* fn = (const unsigned char*)GetProcAddress(module, symbol);
	if (!fn || !InImage(module, fn + at, prefixLen + 4))
		return false;
	int disp;
	memcpy(&disp, fn + at + prefixLen, sizeof(disp));
	const unsigned char* target = fn + at + prefixLen + 4 + disp;
	if (!InImage(module, target, 16))
		return false;
	for (size_t i = 0; i < 4; ++i)
	{
		unsigned int lane;
		memcpy(&lane, target + 4 * i, sizeof(lane));
		if (lane != value)
			return false;
	}
	return true;
}

bool VerifyPassVisibilityLayout(HMODULE ogre)
{
	if (!ogre)
		return false;
	// The byte checks below spell these out; keep the two in step.
	if (PASS_VIEWPORT != 0x18 || PASS_CAMERA != 0x40 || VP_ACT_WIDTH != 0x30 || VP_ACT_HEIGHT != 0x34 ||
	    CAM_AUTO_ASPECT != 0x608 || CAM_FRUSTUM_PLANES != 0x1EC || CAM_GET_ASPECT_SLOT * 8 != 0x120 ||
	    CAM_GET_PLANES_SLOT * 8 != 0x1B0 || QUEUE_POOLS_BEGIN != 0x08 || QUEUE_POOLS_END != 0x10 ||
	    8 + 8 * POOL_WORLD_AABB != 0x20 || 8 + 8 * POOL_VIS_FLAGS != 0x40 || VIS_MASK_BITS != 0x1FFFFFFF ||
	    sizeof(FrustumPlane) != 16 || FRUSTUM_PLANE_COUNT != 6)
		return false;
	if (!GetProcAddress(ogre, SYM_NODE_GET_PASSES) || !GetProcAddress(ogre, SYM_PASS_SCENE_VTABLE))
		return false;
	return BytesAt(ogre, SYM_SM_CULL_PHASE01, CULL_PLANES_AT, CULL_PLANES, sizeof(CULL_PLANES)) &&
	       BytesAt(ogre, SYM_FRUSTUM_PLANES, 0, GET_PLANES, sizeof(GET_PLANES)) &&
	       BytesAt(ogre, SYM_SM_CULL_FRUSTUM, SM_POOLS_AT, SM_POOLS, sizeof(SM_POOLS)) &&
	       BytesAt(ogre, SYM_SM_CULL_FRUSTUM, SM_OBJDATA_AT, SM_OBJDATA, sizeof(SM_OBJDATA)) &&
	       BytesAt(ogre, SYM_SM_CULL_FRUSTUM, SM_COUNT_AT, SM_COUNT, sizeof(SM_COUNT)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_MASK_AT, MO_MASK, sizeof(MO_MASK)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANES_AT, MO_PLANES, sizeof(MO_PLANES)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANE_X_AT, MO_PLANE_X, sizeof(MO_PLANE_X)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANE_Y_AT, MO_PLANE_Y, sizeof(MO_PLANE_Y)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANE_Z_AT, MO_PLANE_Z, sizeof(MO_PLANE_Z)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANE_D_AT, MO_PLANE_D, sizeof(MO_PLANE_D)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_PLANE_NEXT_AT, MO_PLANE_NEXT, sizeof(MO_PLANE_NEXT)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_POOLS_AT, MO_POOLS, sizeof(MO_POOLS)) &&
	       BytesAt(ogre, SYM_MO_CULL_FRUSTUM, MO_LAYER_AT, MO_LAYER, sizeof(MO_LAYER)) &&
	       RipConstantIs(ogre, SYM_MO_CULL_FRUSTUM, MO_LAYER_AT, sizeof(MO_LAYER), LAYER_VISIBILITY) &&
	       BytesAt(ogre, SYM_PASS_SCENE_EXEC, EXEC_CAMERA_AT, EXEC_CAMERA, sizeof(EXEC_CAMERA)) &&
	       BytesAt(ogre, SYM_PASS_SCENE_EXEC, EXEC_CULL_AT, EXEC_CULL, sizeof(EXEC_CULL)) &&
	       BytesAt(ogre, SYM_VP_CULL_PHASE01, VP_ASPECT_AT, VP_ASPECT, sizeof(VP_ASPECT)) &&
	       BytesAt(ogre, SYM_VP_CULL_PHASE01, VP_WIDTH_AT, VP_WIDTH, sizeof(VP_WIDTH)) &&
	       BytesAt(ogre, SYM_VP_CULL_PHASE01, VP_GET_ASPECT_AT, VP_GET_ASPECT, sizeof(VP_GET_ASPECT));
}
