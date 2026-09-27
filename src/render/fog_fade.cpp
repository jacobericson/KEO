#include "render/fog_fade.h"
#include "game/game.h"
#include "base/core.h"
#include <cstring>

typedef void (*FogUpdate_t)(void* controller, void* camera);

static const size_t RENDERER_CAMERA = 0x58;   // Renderer::camera

// The fog update's prologue, and preRenderQueues' call to it, read as two
// overlapping 16-byte windows: mov rax,[au.render]; mov rdx,[rax+58h]
// (camera); mov rcx,[FogController*]; call the thunk, which jumps to the update.
static const unsigned char FOG_UPDATE_PROLOGUE[16] =
	{ 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x54,0x48,0x83 };
static const unsigned char FOG_CALL_ARGS[16] =
	{ 0x48,0x8B,0x05,0x6E,0x5C,0x90,0x01,0x48,0x8B,0x50,0x58,0x48,0x8B,0x0D,0xF3,0xAC };
static const size_t        FOG_CALL_TARGET_AT = 7;   // the second window starts at the camera load
static const unsigned char FOG_CALL_TARGET[16] =
	{ 0x48,0x8B,0x50,0x58,0x48,0x8B,0x0D,0xF3,0xAC,0x8F,0x01,0xE8,0x0D,0x46,0x7E,0xFF };
static const unsigned char FOG_THUNK[5] = { 0xE9,0x74,0x8A,0x0F,0x00 };

static FogUpdate_t s_fogUpdate = NULL;
static bool        s_checked   = false;

static uintptr_t CurrentRenderer()
{
	uintptr_t holder = (uintptr_t)GameAddr(RVA_RENDERER);
	return holder ? *(const uintptr_t*)holder : 0;
}

// The two arguments preRenderQueues passes to the update; false when either
// is missing.
static bool FogArgs(void** controller, void** camera)
{
	uintptr_t renderer = CurrentRenderer();
	*controller = *(void**)GameAddr(RVA_FOG_CONTROLLER);
	*camera = renderer ? *(void**)(renderer + RENDERER_CAMERA) : NULL;
	return *controller && *camera;
}

bool InstallFogFade()
{
	if (s_checked)
		return s_fogUpdate != NULL;
	s_checked = true;
	const unsigned char* callSite = (const unsigned char*)GameAddr(RVA_QUEUE_CUTTER_FOG_CALL);
	if (!VerifyPrologue(RVA_FOG_CONTROLLER_UPDATE, FOG_UPDATE_PROLOGUE, "FogController update") ||
	    !VerifyPrologue(RVA_QUEUE_CUTTER_FOG_CALL, FOG_CALL_ARGS, "preRenderQueues fog call") ||
	    !VerifyPrologueAt(callSite + FOG_CALL_TARGET_AT, FOG_CALL_TARGET, "preRenderQueues fog call target",
	                      "preRenderQueues+0x90A", NULL) ||
	    memcmp(GameAddr(RVA_FOG_UPDATE_THUNK), FOG_THUNK, sizeof(FOG_THUNK)) != 0)
		return false;
	s_fogUpdate = (FogUpdate_t)GameAddr(RVA_FOG_CONTROLLER_UPDATE);
	return true;
}

bool FogFadeAvailable()
{
	void* controller = NULL;
	void* camera = NULL;
	return s_fogUpdate && FogArgs(&controller, &camera);
}

bool FogFadeCompensate(LONG passes)
{
	void* controller = NULL;
	void* camera = NULL;
	if (!s_fogUpdate || !FogArgs(&controller, &camera))
		return false;
	for (LONG i = 0; i < passes; ++i)
		s_fogUpdate(controller, camera);
	return true;
}
