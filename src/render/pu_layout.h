#pragma once
#include <windows.h>

// Kenshi's EffectsManager (exe) and the ParticleUniverse::ParticleSystem
// fields the particle levers read. The exe offsets are fixed by the build the
// version check accepts; the DLL offsets are checked by VerifyParticleLayout.
static const size_t EM_ACTIVE_DATA = 200;    // Ogre::FastArray<Effect*> of active effects: data
static const size_t EM_ACTIVE_SIZE = 208;    //   element count
static const size_t FX_GAMEDATA    = 8;      // Effect: GameData* of the effect record
static const size_t FX_HANDLER     = 24;     // Effect: ParticleSystemHandler* (NULL: no particles)
static const size_t FX_AGE         = 48;     // Effect: float, game seconds since start (0 at start, restored on load)
static const size_t FX_STATE       = 92;     // Effect: int, 0 = running, 1 = stopping, 2 = dead
static const size_t PSH_SYSTEM     = 168;    // ParticleSystemHandler: ParticleUniverse::ParticleSystem*
static const size_t GD_NAME        = 0x28;   // GameData::name (std::string)
static const size_t PS_NONVIS_TIME = 0x278;  // float: the non-visible update timeout, seconds
static const size_t PS_NONVIS_SET  = 0x27C;  // bool: non-visible update timeout set
static const size_t PS_TEMPLATE    = 0x380;  // std::string, ParticleSystem::getTemplateName

// MSVC 2010 std::string: a 16-byte buffer, or a heap pointer once capacity >= 16.
static const size_t STR_SIZE     = 16;
static const size_t STR_CAPACITY = 24;
static const size_t STR_SSO      = 16;

static const char* const PU_DLL   = "Plugin_ParticleUniverse_x64.dll";
static const char* const OGRE_DLL = "OgreMain_x64.dll";
static const char* const SYM_PU_TEMPLATE =
	"?getTemplateName@ParticleSystem@ParticleUniverse@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ";
static const char* const SYM_PU_SET_NONVISIBLE =
	"?setNonVisibleUpdateTimeout@ParticleSystem@ParticleUniverse@@QEAAXM@Z";
static const char* const SYM_PU_UPDATE        = "?_update@ParticleSystem@ParticleUniverse@@QEAAXM@Z";
static const char* const SYM_ROOT_NEXT_FRAME  = "?getNextFrameNumber@Root@Ogre@@QEBAKXZ";

// Checks the instruction bytes that use the DLL-side offsets above; false on
// any mismatch or missing export.
bool VerifyParticleLayout(HMODULE pu, HMODULE ogre);

// Copies an MSVC 2010 std::string into out (truncated, always terminated).
// Leaves out empty when the object does not look like a string.
void ReadStdString(const void* s, char* out, size_t cap);
