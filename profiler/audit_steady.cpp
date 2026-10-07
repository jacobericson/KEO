// audit_steady.cpp - Steady-state cost probes (SteadyDetail=1). Character::update and
// Character::pausedUpdate are timed per call, split player / NPC, with the animation update they
// make timed inside them; the paused update is also split by the character's on-screen and
// visible-update flags at entry; FactionRelations::update is timed on the AI thread with the size
// of the map it walks; the formation lookups and rebuilds are counted on whatever thread runs them.
// The character hooks write the main thread's frame, the relations hook the AI slot inside the AI
// body, the formation hooks Interlocked counters only. No hook takes a lock, allocates, or logs.

#include "audit_steady.h"

namespace audit_steady_detail {

// Which character update is running the animation update (main thread, CurFrame::animParent).
enum AnimParent { ANIM_PARENT_NONE, ANIM_PARENT_UPDATE, ANIM_PARENT_PAUSED };

} // audit_steady_detail
using namespace audit_steady_detail;

namespace kenshiframeaudit_detail {

// Entry detours (Steam 1.0.65).
static const size_t RVA_CHAR_UPDATE = 0x5CE6A0;  // Character::update, vtable +0xE0
static const size_t RVA_CHAR_PAUSED = 0x5C7090;  // Character::pausedUpdate, vtable +0x270
static const size_t RVA_ANIM_UPDATE = 0x5B5E30;  // AnimationClass::update, vtable +0x30 (the human class's slot calls it)
static const size_t RVA_REL_UPDATE  = 0x6B2480;  // FactionRelations::update, vtable +0x10
static const size_t RVA_FORM_LOOKUP = 0x2703B0;  // Blackboard::getFormationPositionOffset
static const size_t RVA_FORM_BUILD  = 0x26DAF0;  // Blackboard::buildFormation, called only by the lookup

static const unsigned char PRO_CHAR_UPDATE[16] = { 0x40,0x56,0x48,0x83,0xEC,0x60,0x48,0x8B,0x01,0x0F,0xB6,0x91,0xE5,0x00,0x00,0x00 };
static const unsigned char PRO_CHAR_PAUSED[16] = { 0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0x01,0x0F,0xB6,0x91,0xE5,0x00,0x00,0x00 };
static const unsigned char PRO_ANIM_UPDATE[16] = { 0x4C,0x8B,0xDC,0x57,0x48,0x81,0xEC,0xD0,0x00,0x00,0x00,0x48,0xC7,0x44,0x24,0x78 };
static const unsigned char PRO_REL_UPDATE[16]  = { 0x48,0x83,0x79,0x40,0x00,0x4C,0x8B,0xC1,0x74,0x13,0x48,0x8B,0x51,0x38,0x48,0x8B };
static const unsigned char PRO_FORM_LOOKUP[16] = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 };
static const unsigned char PRO_FORM_BUILD[16]  = { 0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xD8 };

// What the hooks read.
static const size_t CHAR_OWNER        = 0x10;   // RootObjectBase::owner, Faction*
static const size_t FACTION_IS_PLAYER = 0x250;  // Faction::isPlayer, PlayerInterface* (NULL for NPC factions)
static const size_t CHAR_VIS_UPDATE   = 0xE4;   // Character::isVisibleUpdateMode, the running loop's full-update test
static const size_t CHAR_ON_SCREEN    = 0x1A9;  // Character::isOnScreen, distance and frustum only
static const size_t REL_TABLE_SIZE    = 0x40;   // FactionRelations::_factionRelations (a map at +0x20): its size_
static_assert(REL_TABLE_SIZE == 0x20 + KLIB_OFF_DeathMapTable_size_, "REL_TABLE_SIZE composed parity");

typedef void        (*CharUpdate_t)(void*);
typedef void        (*AnimUpdate_t)(void*, float);
typedef void        (*RelUpdate_t)(void*);
typedef const void* (*FormLookup_t)(void*, void*);
typedef void        (*FormBuild_t)(void*);

static CharUpdate_t oCharUpdate = NULL;
static CharUpdate_t oCharPaused = NULL;
static AnimUpdate_t oAnimUpdate = NULL;
static RelUpdate_t  oRelUpdate  = NULL;
static FormLookup_t oFormLookup = NULL;
static FormBuild_t  oFormBuild  = NULL;

static bool s_charHooked, s_pausedHooked, s_animHooked, s_relHooked, s_lookupHooked, s_buildHooked;

// Formation counters: any thread adds; the main thread turns the totals into per-frame deltas.
static volatile LONG   s_formLookups    = 0;
static volatile LONG   s_formBuilds     = 0;
static volatile LONG64 s_formBuildTicks = 0;
static LONG   s_prevLookups = 0;   // main thread
static LONG   s_prevBuilds  = 0;
static LONG64 s_prevTicks   = 0;

static bool IsPlayerCharacter(const void* ch)
{
	uintptr_t owner = *(const uintptr_t*)((const char*)ch + CHAR_OWNER);
	return PlausiblePtr(owner) && *(const uintptr_t*)(owner + FACTION_IS_PLAYER) != 0;
}

// Times one character update on the main thread, as the parent of the animation update it makes.
static LONGLONG TimeCharacter(CharUpdate_t orig, void* ch, int parent, int slot, int playerSlot,
                              int* calls, int* playerCalls)
{
	bool player = IsPlayerCharacter(ch);
	int outer = g_cur.animParent;
	g_cur.animParent = parent;
	LONGLONG t0 = Now();
	orig(ch);
	LONGLONG d = Now() - t0;
	g_cur.animParent = outer;
	g_cur.sd[slot] += d;
	++*calls;
	if (player)
	{
		g_cur.sd[playerSlot] += d;
		++*playerCalls;
	}
	return d;
}

static void hk_CharUpdate(void* ch)
{
	if (IsMain() && g_cur.open)
		TimeCharacter(oCharUpdate, ch, ANIM_PARENT_UPDATE, SDT_CU, SDT_CUPLAYER, &g_cur.cuN, &g_cur.cuPlayerN);
	else
		oCharUpdate(ch);
}

static void hk_CharPaused(void* ch)
{
	if (!IsMain() || !g_cur.open)
	{
		oCharPaused(ch);
		return;
	}
	// Read before the original: its setVisible can lower isOnScreen.
	bool onScreen  = *((const unsigned char*)ch + CHAR_ON_SCREEN) != 0;
	bool visUpdate = *((const unsigned char*)ch + CHAR_VIS_UPDATE) != 0;
	LONGLONG d = TimeCharacter(oCharPaused, ch, ANIM_PARENT_PAUSED, SDT_CP, SDT_CPPLAYER, &g_cur.cpN, &g_cur.cpPlayerN);
	if (onScreen)
	{
		g_cur.sd[SDT_CPON] += d;
		++g_cur.cpOnN;
	}
	if (visUpdate)
	{
		g_cur.sd[SDT_CPVIS] += d;
		++g_cur.cpVisN;
	}
}

static void hk_AnimUpdate(void* anim, float dt)
{
	int parent = (IsMain() && g_cur.open) ? g_cur.animParent : ANIM_PARENT_NONE;
	if (parent == ANIM_PARENT_NONE)
	{
		oAnimUpdate(anim, dt);
		return;
	}
	LONGLONG t0 = Now();
	oAnimUpdate(anim, dt);
	g_cur.sd[parent == ANIM_PARENT_UPDATE ? SDT_CUANIM : SDT_CPANIM] += Now() - t0;
}

// Inside the AI body only: g_ai is the AI thread's own slot until it publishes doneSeq.
static void hk_RelUpdate(void* rel)
{
	DWORD ai = g_aiThreadId;
	if (ai == 0 || GetCurrentThreadId() != ai)
	{
		oRelUpdate(rel);
		return;
	}
	size_t nodes = *(const size_t*)((const char*)rel + REL_TABLE_SIZE);
	LONGLONG t0 = Now();
	oRelUpdate(rel);
	g_ai.rel += Now() - t0;
	++g_ai.relCalls;
	g_ai.relNodes += (int)nodes;
}

static const void* hk_FormLookup(void* bb, void* ch)
{
	InterlockedIncrement(&s_formLookups);
	return oFormLookup(bb, ch);
}

static void hk_FormBuild(void* bb)
{
	LONGLONG t0 = Now();
	oFormBuild(bb);
	InterlockedExchangeAdd64(&s_formBuildTicks, Now() - t0);
	InterlockedIncrement(&s_formBuilds);
}

bool SteadySiteTag(int tag)
{
	return (tag >= ST_CH_PERIODIC && tag <= ST_LZ_CHAR) || tag == ST_AF_PLATOONU;
}

void InstallSteadyHooks()
{
	s_charHooked   = AuditHookExe("charUpdate", RVA_CHAR_UPDATE, PRO_CHAR_UPDATE,
	                              (void*)&hk_CharUpdate, (void**)&oCharUpdate);
	s_pausedHooked = AuditHookExe("charPausedUpdate", RVA_CHAR_PAUSED, PRO_CHAR_PAUSED,
	                              (void*)&hk_CharPaused, (void**)&oCharPaused);
	s_animHooked   = AuditHookExe("animUpdate", RVA_ANIM_UPDATE, PRO_ANIM_UPDATE,
	                              (void*)&hk_AnimUpdate, (void**)&oAnimUpdate);
	s_relHooked    = AuditHookExe("factionRelationsUpdate", RVA_REL_UPDATE, PRO_REL_UPDATE,
	                              (void*)&hk_RelUpdate, (void**)&oRelUpdate);
	s_lookupHooked = AuditHookExe("formationLookup", RVA_FORM_LOOKUP, PRO_FORM_LOOKUP,
	                              (void*)&hk_FormLookup, (void**)&oFormLookup);
	s_buildHooked  = AuditHookExe("formationBuild", RVA_FORM_BUILD, PRO_FORM_BUILD,
	                              (void*)&hk_FormBuild, (void**)&oFormBuild);
}

const char* SteadyStatus()
{
	if (!g_cfg.steadyDetail)
		return "off";
	bool all = s_charHooked && s_pausedHooked && s_animHooked && s_relHooked && s_lookupHooked && s_buildHooked;
	for (int tag = ST_CH_PERIODIC; tag <= ST_LZ_CHAR; ++tag)
		all = all && g_haveTag[tag];
	all = all && g_haveTag[ST_AF_PLATOONU];
	return all ? "on" : "partial";
}

void SteadyCollectAi(const ThreadSlot& s)
{
	if (s_relHooked)
	{
		g_cur.relMs    = TicksToMs(s.rel);
		g_cur.relCalls = s.relCalls;
		g_cur.relNodes = s.relNodes;
	}
	if (g_haveTag[ST_AF_PLATOONU])
		g_cur.afPlatoonU = TicksToMs(s.platU);
}

static float SdMs(const CurFrame& c, int slot, bool have)
{
	return have ? TicksToMs(c.sd[slot]) : Nan();
}

void SteadyFrameTotals(const CurFrame& c, FrameRec& r)
{
	r.m[M_SD_CU]         = SdMs(c, SDT_CU, s_charHooked);
	r.m[M_SD_CUPLAYER]   = SdMs(c, SDT_CUPLAYER, s_charHooked);
	r.m[M_SD_CUANIM]     = SdMs(c, SDT_CUANIM, s_charHooked && s_animHooked);
	r.m[M_SD_CP]         = SdMs(c, SDT_CP, s_pausedHooked);
	r.m[M_SD_CPPLAYER]   = SdMs(c, SDT_CPPLAYER, s_pausedHooked);
	r.m[M_SD_CPANIM]     = SdMs(c, SDT_CPANIM, s_pausedHooked && s_animHooked);
	r.m[M_SD_CHPERIODIC] = SdMs(c, SDT_CHPERIODIC, g_haveTag[ST_CH_PERIODIC]);
	r.m[M_SD_CHFOUR]     = SdMs(c, SDT_CHFOUR, g_haveTag[ST_CH_FOUR]);
	r.m[M_SD_CHPOST]     = SdMs(c, SDT_CHPOST, g_haveTag[ST_CH_POST]);
	r.m[M_SD_CHDEATH]    = SdMs(c, SDT_CHDEATH, g_haveTag[ST_CH_DEATH]);
	r.m[M_SD_FCUPDATE]   = SdMs(c, SDT_FCUPDATE, g_haveTag[ST_FC_UPDATE]);
	r.m[M_SD_FCACTIVE]   = SdMs(c, SDT_FCACTIVE, g_haveTag[ST_FC_ACTIVE]);
	r.m[M_SD_FCPERIODIC] = SdMs(c, SDT_FCPERIODIC, g_haveTag[ST_FC_PERIODIC]);
	r.m[M_SD_CPON]       = SdMs(c, SDT_CPON, s_pausedHooked);
	r.m[M_SD_CPVIS]      = SdMs(c, SDT_CPVIS, s_pausedHooked);
	r.m[M_SD_CHLIGHT]    = SdMs(c, SDT_CHLIGHT, g_haveTag[ST_CH_LIGHT]);
	r.m[M_SD_LZZONES]    = SdMs(c, SDT_LZZONES, g_haveTag[ST_LZ_ZONES]);
	r.m[M_SD_LZBLD]      = SdMs(c, SDT_LZBLD, g_haveTag[ST_LZ_BLD]);
	r.m[M_SD_LZCHAR]     = SdMs(c, SDT_LZCHAR, g_haveTag[ST_LZ_CHAR]);
	r.m[M_AI_REL]        = c.relMs;
	r.m[M_AI_PLATU]      = c.afPlatoonU;
	r.c[C_CUN]           = c.cuN;
	r.c[C_CUPLAYERN]     = c.cuPlayerN;
	r.c[C_CPN]           = c.cpN;
	r.c[C_CPPLAYERN]     = c.cpPlayerN;
	r.c[C_RELCALLS]      = c.relCalls;
	r.c[C_RELNODES]      = c.relNodes;
	r.c[C_CPONN]         = c.cpOnN;
	r.c[C_CPVISN]        = c.cpVisN;
	r.c[C_CHPERIODICN]   = c.chPeriodicN;
	r.c[C_CHLIGHTN]      = c.chLightN;
	r.c[C_LZCALLS]       = c.lzCalls;
	r.c[C_LZZONEN]       = c.lzZoneN;
	r.c[C_LZBLDN]        = c.lzBldN;
	r.c[C_LZLIGHTN]      = c.lzLightN;
	r.c[C_LZCHARN]       = c.lzCharN;
	r.c[C_LZCHARLIGHTN]  = c.lzCharLightN;
	if (s_lookupHooked)
	{
		LONG n = InterlockedCompareExchange(&s_formLookups, 0, 0);
		r.c[C_FMLOOKUPS] = (int)(n - s_prevLookups);
		s_prevLookups = n;
	}
	if (s_buildHooked)
	{
		LONG   n = InterlockedCompareExchange(&s_formBuilds, 0, 0);
		LONG64 t = InterlockedCompareExchange64(&s_formBuildTicks, 0, 0);
		r.c[C_FMBUILDS]   = (int)(n - s_prevBuilds);
		r.m[M_SD_FMBUILD] = TicksToMs(t - s_prevTicks);
		s_prevBuilds = n;
		s_prevTicks  = t;
	}
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
