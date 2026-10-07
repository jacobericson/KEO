// audit_install.cpp - Frame audit hook and probe installation.
// Startup thread installs hooks; diagnostics queue under g_lineCS alone.

#include "audit_detail.h"
#include "audit_steady.h"

#include "base/klib_include.h"
#include <core/Functions.h>
#include <Debug.h>
#include "base/klib_include_end.h"

namespace kenshiframeaudit_detail {
// =========================================================================
// Install helpers
// =========================================================================

bool InModule(HMODULE module, const void* p, size_t len)
{
	if (!module || !p)
		return false;
	uintptr_t base = (uintptr_t)module;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
	uintptr_t a = (uintptr_t)p;
	return a >= base && a + len <= end;
}

std::string HexBytes(const unsigned char* b)
{
	std::string s;
	for (int i = 0; i < 16; ++i)
		s += Fmt(i ? " %02X" : "%02X", (unsigned)b[i]);
	return s;
}

// Same rules as the optimizer's build gate (src/base/prologue.cpp VerifyPrologue):
// an exact match, or another plugin's detour (E9 rel32 / FF 25 rel32) whose
// remaining bytes still match, allowing 0x90/0xCC padding up to offset 8.
int DetourLength(const unsigned char* b)
{
	if (b[0] == 0xE9) return 5;
	if (b[0] == 0xFF && b[1] == 0x25) return 6;
	return 0;
}

bool PrologueTailMatches(const unsigned char* actual, const unsigned char* expect, int start)
{
	if (memcmp(actual + start, expect + start, 16 - start) == 0)
		return true;
	for (int i = start; i < 8; ++i)
	{
		if (actual[i] != 0x90 && actual[i] != 0xCC)
			return false;
	}
	return memcmp(actual + 8, expect + 8, 8) == 0;
}

// Returns "ok", "shared" or a SKIP reason.
std::string CheckPrologue(HMODULE module, const void* target, const unsigned char* expect)
{
	if (!InModule(module, target, 16))
		return "SKIP target outside module";
	const unsigned char* actual = (const unsigned char*)target;
	if (memcmp(actual, expect, 16) == 0)
		return "ok";
	int detour = DetourLength(actual);
	if (detour > 0 && PrologueTailMatches(actual, expect, detour))
		return "shared";
	return "SKIP prologue expected " + HexBytes(expect) + " found " + HexBytes(actual);
}

int g_hooksOk = 0, g_hooksTotal = 0;

bool HookAt(const char* name, HMODULE module, void* target, const unsigned char* expect,
            void* detour, void** orig)
{
	++g_hooksTotal;
	if (NameDisabled(name))
	{
		AuditLine(Fmt("[Audit] hook %s SKIP ini", name));
		return false;
	}
	if (!target)
	{
		AuditLine(Fmt("[Audit] hook %s SKIP not found", name));
		return false;
	}
	std::string check = CheckPrologue(module, target, expect);
	if (check != "ok" && check != "shared")
	{
		AuditLine(Fmt("[Audit] hook %s @%p ", name, target) + check);
		return false;
	}
	if (KenshiLib::AddHook(target, detour, orig) != KenshiLib::SUCCESS)
	{
		AuditLine(Fmt("[Audit] hook %s @%p SKIP AddHook failed", name, target));
		return false;
	}
	++g_hooksOk;
	AuditLine(Fmt("[Audit] hook %s @%p %s", name, target, check.c_str()));
	return true;
}

void* ExeAddr(size_t rva)
{
	return (void*)KlibAddress(g_base, rva);
}

void InstallOgreHooks(HMODULE ogre, bool* haveRenderOneFrame)
{
	*haveRenderOneFrame = false;
	if (!ogre)
	{
		AuditLine("[Audit] OgreMain_x64.dll not loaded: Ogre hooks skipped");
		return;
	}
	g_getStage   = (GetRenderStage_t)GetProcAddress(ogre, SYM_GET_RENDER_STAGE);
	g_getWorkers = (GetWorkerThreads_t)GetProcAddress(ogre, SYM_GET_WORKER_THREADS);

	if (g_cfg.boundaryRenderOneFrame)
		*haveRenderOneFrame = HookAt("renderOneFrame", ogre,
			(void*)GetProcAddress(ogre, SYM_RENDER_ONE_FRAME), PRO_RENDER_ONE_FRAME,
			(void*)&hk_RenderOneFrame, (void**)&oRenderOneFrame);

	if (g_cfg.ogreSplit)
	{
		HookAt("updateSceneGraph", ogre, (void*)GetProcAddress(ogre, SYM_UPDATE_SCENE_GRAPH),
		       PRO_UPDATE_SCENE, (void*)&hk_UpdateScene, (void**)&oUpdateScene);
		g_cmHooked = HookAt("compositorUpdate", ogre, (void*)GetProcAddress(ogre, SYM_CM2_UPDATE),
		                    PRO_CM2_UPDATE, (void*)&hk_Cm2Update, (void**)&oCm2Update);
		HookAt("swapAllFinalTargets", ogre, (void*)GetProcAddress(ogre, SYM_CM2_SWAP),
		       PRO_CM2_SWAP, (void*)&hk_Cm2Swap, (void**)&oCm2Swap);
	}
	bool drawHook = false;
	if (g_cfg.draws)
		drawHook = HookAt("renderSystemRender", ogre, (void*)GetProcAddress(ogre, SYM_RS_RENDER),
		                  PRO_RS_RENDER, (void*)&hk_RsRender, (void**)&oRsRender);

	// Scene-call breakdown and draw attribution need the draw hook as well.
	if (g_cfg.renderDetail && drawHook)
	{
		g_moGetName  = (OgreGetter_t)GetProcAddress(ogre, SYM_MO_GET_NAME);
		g_subParent  = (OgreGetter_t)GetProcAddress(ogre, SYM_SUBENT_PARENT);
		g_entMesh    = (OgreGetter_t)GetProcAddress(ogre, SYM_ENT_GET_MESH);
		g_batchMesh  = (OgreGetter_t)GetProcAddress(ogre, SYM_BATCH_MESH_REF);
		g_resGetName = (OgreGetter_t)GetProcAddress(ogre, SYM_RES_GET_NAME);
		g_vpTarget   = (OgreGetter_t)GetProcAddress(ogre, SYM_VP_TARGET);
		g_rtGetName  = (OgreGetter_t)GetProcAddress(ogre, SYM_RT_GET_NAME);
		g_vpWidth    = (OgreIntGetter_t)GetProcAddress(ogre, SYM_VP_WIDTH);
		g_vpHeight   = (OgreIntGetter_t)GetProcAddress(ogre, SYM_VP_HEIGHT);
		AuditLine(Fmt("[Audit] Ogre accessors: moName=%d subParent=%d entMesh=%d batchMesh=%d resName=%d vpTarget=%d rtName=%d vpSize=%d",
		              g_moGetName != NULL, g_subParent != NULL, g_entMesh != NULL, g_batchMesh != NULL,
		              g_resGetName != NULL, g_vpTarget != NULL, g_rtGetName != NULL,
		              g_vpWidth != NULL && g_vpHeight != NULL));

		bool a = HookAt("renderPhase02", ogre, (void*)GetProcAddress(ogre, SYM_RENDER_PHASE02),
		                PRO_RENDER_PHASE02, (void*)&hk_RenderPhase02, (void**)&oRenderPhase02);
		bool b = HookAt("cullPhase01", ogre, (void*)GetProcAddress(ogre, SYM_CULL_PHASE01),
		                PRO_CULL_PHASE01, (void*)&hk_CullPhase01, (void**)&oCullPhase01);
		bool c = HookAt("renderVisibleObjects", ogre, (void*)GetProcAddress(ogre, SYM_RENDER_VISIBLE),
		                PRO_RENDER_VISIBLE, (void*)&hk_RenderVisible, (void**)&oRenderVisible);
		bool d = HookAt("renderSingleObject", ogre, (void*)GetProcAddress(ogre, SYM_RSO),
		                PRO_RSO, (void*)&hk_Rso, (void**)&oRso);
		bool e = HookAt("setPass", ogre, (void*)GetProcAddress(ogre, SYM_SET_PASS),
		                PRO_SET_PASS, (void*)&hk_SetPass, (void**)&oSetPass);
		// A partial set would attribute time to the wrong place: all or nothing
		// (hooks that did install then just pass through).
		g_renderOn = a && b && c && d && e;

		// Optional splits of that time; each is reported only if it installed.
		// They add per-draw work of their own: RenderDeep=0 leaves them out.
		g_getVisFlags = (VisFlags_t)GetProcAddress(ogre, SYM_GET_VIS_FLAGS);
		if (g_renderOn && g_cfg.renderDeep)
		{
			g_syncHooked = HookAt("barrierSync", ogre, (void*)GetProcAddress(ogre, SYM_BARRIER_SYNC),
			                      PRO_BARRIER_SYNC, (void*)&hk_BarrierSync, (void**)&oBarrierSync);
			g_oldAnimHooked = HookAt("updateAllOldAnimations", ogre, (void*)GetProcAddress(ogre, SYM_OLD_ANIMS),
			                         PRO_OLD_ANIMS, (void*)&hk_OldAnims, (void**)&oOldAnims);
			HMODULE d3d = GetModuleHandleA(D3D11_DLL);
			if (d3d)
			{
				g_bindHooked = HookAt("d3dBindParams", d3d, (void*)((uintptr_t)d3d + RVA_D3D_BIND_PARAMS),
				                      PRO_D3D_BIND, (void*)&hk_D3DBind, (void**)&oD3DBind);
				g_d3dHooked = HookAt("d3dRender", d3d, (void*)((uintptr_t)d3d + RVA_D3D_RENDER),
				                     PRO_D3D_RENDER, (void*)&hk_D3DRender, (void**)&oD3DRender);
			}
			else
				AuditLine("[Audit] RenderSystem_Direct3D11_x64.dll not loaded: D3D11 hooks skipped");
		}
	}
}

// Checks the ParticleUniverse / Root field offsets against the code that uses
// them, then detours ParticleUniverse::ParticleSystem::_update. The census
// (exe side) starts later, once the `particles` site is installed.
void InstallParticleHooks(HMODULE ogre)
{
	if (!g_cfg.particles)
	{
		AuditLine("[Audit] particle counters off (Particles=0)");
		return;
	}
	HMODULE pu = GetModuleHandleA(PU_DLL);
	if (!pu || !ogre)
	{
		AuditLine("[Audit] Plugin_ParticleUniverse_x64.dll or OgreMain_x64.dll not loaded: particle counters off");
		return;
	}
	const unsigned char* upd  = (const unsigned char*)GetProcAddress(pu, SYM_PU_UPDATE);
	const unsigned char* name = (const unsigned char*)GetProcAddress(pu, SYM_PU_TEMPLATE);
	const unsigned char* next = (const unsigned char*)GetProcAddress(ogre, SYM_ROOT_NEXT_FRAME);
	g_rootSingleton = (RootSingleton_t)GetProcAddress(ogre, SYM_ROOT_SINGLETON);
	g_puParticles   = (PuParticles_t)GetProcAddress(pu, SYM_PU_PARTICLES);
	g_fxLayoutOk = upd && name && next && g_rootSingleton &&
	               InModule(pu, upd, PU_UPDATE_VISTEST + sizeof(PU_VISTEST_BYTES)) &&
	               memcmp(upd + PU_UPDATE_VISTEST, PU_VISTEST_BYTES, sizeof(PU_VISTEST_BYTES)) == 0 &&
	               InModule(pu, name, sizeof(PU_TEMPLATE_BYTES)) &&
	               memcmp(name, PU_TEMPLATE_BYTES, sizeof(PU_TEMPLATE_BYTES)) == 0 &&
	               InModule(ogre, next, sizeof(ROOT_FRAME_BYTES)) &&
	               memcmp(next, ROOT_FRAME_BYTES, sizeof(ROOT_FRAME_BYTES)) == 0;
	if (!g_fxLayoutOk)
	{
		AuditLine("[Audit] particle counters off: ParticleUniverse / Root field layout does not match this build");
		return;
	}
	g_root = (const char*)g_rootSingleton();   // NULL until Root exists; the census retries
	g_fxHooked = HookAt("puUpdate", pu, (void*)upd, PRO_PU_UPDATE,
	                    (void*)&hk_PuUpdate, (void**)&oPuUpdate);
	AuditLine(Fmt("[Audit] particle accessors: root=%d particles=%d", g_root != NULL, g_puParticles != NULL));
}

bool InstallExeHooks()
{
	HMODULE exe = (HMODULE)g_base;
	bool started = HookAt("frameStarted", exe, ExeAddr(RVA_FRAME_STARTED), PRO_FRAME_STARTED,
	                      (void*)&hk_FrameStarted, (void**)&oFrameStarted);
	bool queued  = HookAt("frameRenderingQueued", exe, ExeAddr(RVA_FRAME_QUEUED), PRO_FRAME_QUEUED,
	                      (void*)&hk_FrameQueued, (void**)&oFrameQueued);
	bool ended   = HookAt("frameEnded", exe, ExeAddr(RVA_FRAME_ENDED), PRO_FRAME_ENDED,
	                      (void*)&hk_FrameEnded, (void**)&oFrameEnded);
	bool listeners = started && queued && ended;

	HookAt("aiBody", exe, ExeAddr(RVA_AI_BODY), PRO_AI_BODY,
	       (void*)&hk_AiBody, (void**)&oAiBody);
	if (g_cfg.threadBodies || g_cfg.physxDetail)
	{
		g_physBodyHooked = HookAt("physicsBody", exe, ExeAddr(RVA_PHYS_BODY), PRO_PHYS_BODY,
		                           (void*)&hk_PhysBody, (void**)&oPhysBody);
	}
	if (g_cfg.threadBodies)
	{
		HookAt("birdsBody", exe, ExeAddr(RVA_BIRDS_BODY), PRO_BIRDS_BODY,
		       (void*)&hk_BirdsBody, (void**)&oBirdsBody);
	}
	bool physUT = HookAt("physUT", exe, ExeAddr(RVA_PHYS_UT), PRO_PHYS_UT,
	                     (void*)&hk_PhysUT, (void**)&oPhysUT);
	if (g_cfg.hullDiag)
		g_hullStatus = Hulls_Install(g_base, g_exeEnd, KlibAddress(g_base, RVA_GAMEWORLD), physUT,
		                             g_dllDir + "audit\\", g_runName);
	if (g_cfg.physxDetail)
	{
		HookAt("isIndoors", exe, ExeAddr(RVA_IS_INDOORS), PRO_IS_INDOORS,
		       (void*)&hk_IsIndoors, (void**)&oIsIndoors);
		bool makeHull = HookAt("physMakeHull", exe, ExeAddr(RVA_PHYS_MAKE_HULL), PRO_PHYS_MAKE_HULL,
		                           (void*)&hk_PhysMakeHull, (void**)&oPhysMakeHull);
		bool makeFile = HookAt("physMakeFile", exe, ExeAddr(RVA_PHYS_MAKE_FILE), PRO_PHYS_MAKE_FILE,
		                           (void*)&hk_PhysMakeFile, (void**)&oPhysMakeFile);
		bool makeScythe = HookAt("physMakeScythe", exe, ExeAddr(RVA_PHYS_MAKE_SCYTHE), PRO_PHYS_MAKE_SCYTHE,
		                             (void*)&hk_PhysMakeScythe, (void**)&oPhysMakeScythe);
		bool finishScythe = HookAt("physFinishScythe", exe, ExeAddr(RVA_PHYS_FINISH_SCYTHE), PRO_PHYS_FINISH_SCYTHE,
		                               (void*)&hk_PhysFinishScythe, (void**)&oPhysFinishScythe);
		g_physMakeHooks = makeHull && makeFile && makeScythe && finishScythe;
		bool applyHull = HookAt("physApplyHull", exe, ExeAddr(RVA_PHYS_APPLY_HULL), PRO_PHYS_APPLY_HULL,
		                            (void*)&hk_PhysApplyHull, (void**)&oPhysApplyHull);
		bool applyDoor = HookAt("physApplyDoor", exe, ExeAddr(RVA_PHYS_APPLY_DOOR), PRO_PHYS_APPLY_DOOR,
		                            (void*)&hk_PhysApplyDoor, (void**)&oPhysApplyDoor);
		g_physApplyHooks = applyHull && applyDoor;
	}
	if (g_cfg.aiLists)
	{
		g_bodyHooked = HookAt("charBodyUpdate", exe, ExeAddr(RVA_CHARBODY_UPD), PRO_CHARBODY_UPD,
		                      (void*)&hk_BodyUpdate, (void**)&oBodyUpdate);
		g_moveHooked = HookAt("charMovementUpdate", exe, ExeAddr(RVA_CHARMOVE_UPD), PRO_CHARMOVE_UPD,
		                      (void*)&hk_MoveUpdate, (void**)&oMoveUpdate);
	}
	if (g_cfg.steadyDetail)
		InstallSteadyHooks();
	HookAt("zoneLifecycle", exe, ExeAddr(RVA_ZONE_LIFECYCLE), PRO_ZONE_LIFECYCLE,
	       (void*)&hk_ZoneLifecycle, (void**)&oZoneLifecycle);

	if (g_cfg.saveDetail)
	{
		// SaveManager::saveGame is the one that must install: without it no stage
		// hook ever leaves its pass-through branch, so a partial set is harmless.
		g_saveHooked =
		    HookAt("smSaveGame", exe, ExeAddr(RVA_SM_SAVEGAME), PRO_SM_SAVEGAME,
		           (void*)&hk_SmSaveGame, (void**)&oSmSaveGame);
		HookAt("smUpdateAutoSave", exe, ExeAddr(RVA_SM_UPDATEAUTO), PRO_SM_UPDATEAUTO,
		       (void*)&hk_SmUpdateAuto, (void**)&oSmUpdateAuto);
		HookAt("zmSaveZoneStates", exe, ExeAddr(RVA_ZM_SAVESTATES), PRO_ZM_SAVESTATES,
		       (void*)&hk_ZmSaveStates, (void**)&oZmSaveStates);
		HookAt("zmcSaveLevelData", exe, ExeAddr(RVA_ZMC_SAVELEVEL), PRO_ZMC_SAVELEVEL,
		       (void*)&hk_ZmcSaveLevel, (void**)&oZmcSaveLevel);
		HookAt("rocSerialiseThings", exe, ExeAddr(RVA_ROC_SERIALISE), PRO_ROC_SERIALISE,
		       (void*)&hk_RocSerialise, (void**)&oRocSerialise);
		HookAt("zmcSaveItems", exe, ExeAddr(RVA_ZMC_SAVEITEMS), PRO_ZMC_SAVEITEMS,
		       (void*)&hk_ZmcSaveItems, (void**)&oZmcSaveItems);
		HookAt("zmcSaveToDisk", exe, ExeAddr(RVA_ZMC_SAVEDISK), PRO_ZMC_SAVEDISK,
		       (void*)&hk_ZmcSaveDisk, (void**)&oZmcSaveDisk);
		HookAt("gdcSave", exe, ExeAddr(RVA_GDC_SAVE), PRO_GDC_SAVE,
		       (void*)&hk_GdcSave, (void**)&oGdcSave);
		HookAt("tlSaveUniqueTowns", exe, ExeAddr(RVA_TL_SAVEUNIQUE), PRO_TL_SAVEUNIQUE,
		       (void*)&hk_TlSaveUnique, (void**)&oTlSaveUnique);
		HookAt("tlSaveState", exe, ExeAddr(RVA_TL_SAVESTATE), PRO_TL_SAVESTATE,
		       (void*)&hk_TlSaveState, (void**)&oTlSaveState);
		HookAt("fmSavePlayerState", exe, ExeAddr(RVA_FM_SAVEPLAYER), PRO_FM_SAVEPLAYER,
		       (void*)&hk_FmSavePlayer, (void**)&oFmSavePlayer);
		HookAt("fmSaveGameState", exe, ExeAddr(RVA_FM_SAVESTATE), PRO_FM_SAVESTATE,
		       (void*)&hk_FmSaveState, (void**)&oFmSaveState);
		HookAt("pmSaveTexture", exe, ExeAddr(RVA_PM_SAVETEX), PRO_PM_SAVETEX,
		       (void*)&hk_PmSaveTex, (void**)&oPmSaveTex);
		HookAt("sfsSaveGame", exe, ExeAddr(RVA_SFS_SAVEGAME), PRO_SFS_SAVEGAME,
		       (void*)&hk_SfsSaveGame, (void**)&oSfsSaveGame);
		HookAt("sfsSync", exe, ExeAddr(RVA_SFS_SYNC), PRO_SFS_SYNC,
		       (void*)&hk_SfsSync, (void**)&oSfsSync);
		AuditLine(Fmt("[Audit] save detail: saveGame=%d top=%d",
		              g_saveHooked ? 1 : 0, g_cfg.saveTop));
	}
	return listeners;
}

int InstallSites(int* total)
{
	// Sites the INI turns off, and the per-character visibility probes unless
	// AiVis=1, are left out of the table given to CallSiteProbe.
	std::vector<CallSiteProbe::Site> wanted;
	std::vector<int> index;
	for (int i = 0; i < NUM_SITES; ++i)
	{
		int tag = g_sites[i].tag;
		bool vis = (tag == ST_AIVIS1 || tag == ST_AIVIS2);
		if (vis && !g_cfg.aiVis)
			continue;
		bool lists = (tag == ST_AITU || tag == ST_AITU4 || tag == ST_AITUP ||
		              tag == ST_AIFLUSH || tag == ST_AIANIM);
		if (lists && !g_cfg.aiLists)
			continue;
		bool physx = tag >= ST_PHYS_DETAIL_FIRST;
		if (physx && !g_cfg.physxDetail)
			continue;
		if (SteadySiteTag(tag) && !g_cfg.steadyDetail)
			continue;
		if (NameDisabled(g_sites[i].name))
		{
			AuditLine(Fmt("[Audit] site %s @0x%X SKIP ini", g_sites[i].name, (unsigned)g_sites[i].siteRva));
			++*total;
			continue;
		}
		// A worker body must not be running while its call sites are patched.
		if (tag >= ST_AI_FIRST)
		{
			bool phys = tag >= ST_PHYS_FIRST;
			uintptr_t th = *(const uintptr_t*)((phys ? KLIB_MEMBER(5, KlibAddress(g_base, RVA_GAMEWORLD), GameWorld_physics, GW_PHYSICS) : KLIB_MEMBER(5, KlibAddress(g_base, RVA_GAMEWORLD), GameWorld__AINonRenderThread, GW_AITHREAD)));
			if (PlausiblePtr(th) && *(const unsigned char*)(KLIB_MEMBER(5, th, ThreadClass__running, THREAD_RUNNING)))
			{
				AuditLine(Fmt("[Audit] site %s @0x%X SKIP %s-running", g_sites[i].name,
				              (unsigned)g_sites[i].siteRva, phys ? "physics" : "ai"));
				++*total;
				continue;
			}
		}
		wanted.push_back(g_sites[i]);
		index.push_back(i);
	}
	*total += (int)wanted.size();
	if (wanted.empty())
		return 0;

	CallSiteProbe::SetCallbacks(&OnProbeEnter, &OnProbeExit);
	int ok = CallSiteProbe::Install((HMODULE)g_base, &wanted[0], (int)wanted.size());
	for (size_t i = 0; i < wanted.size(); ++i)
	{
		g_sites[index[i]] = wanted[i];
		if (wanted[i].id >= 0 && wanted[i].tag >= 0 && wanted[i].tag < ST_COUNT)
			g_haveTag[wanted[i].tag] = true;
		std::string target = wanted[i].vslot
			? Fmt("[vt+0x%X]", (unsigned)wanted[i].vslot)
			: Fmt(wanted[i].indirect ? "[0x%X]" : "0x%X", (unsigned)wanted[i].targetRva);
		AuditLine(Fmt("[Audit] site %s @0x%X -> %s %s", wanted[i].name,
		              (unsigned)wanted[i].siteRva, target.c_str(), wanted[i].status));
	}
	return ok;
}

// Checks the traceAll code the hit-list reader and the shadow rays rely on,
// resolves the input globals, and arms the split. Runs after InstallSites.
void InstallCursor(bool steam)
{
	memset(&g_cursor, 0, sizeof(g_cursor));
	if (steam)
	{
		g_cursor.gw         = KlibAddress(g_base, RVA_GAMEWORLD);
		g_cursor.key        = KlibAddress(g_base, RVA_INPUT_KEY);
		g_cursor.prevMLeft  = KlibAddress(g_base, RVA_PREV_MLEFT);
		g_cursor.prevMRight = KlibAddress(g_base, RVA_PREV_MRIGHT);
	}
	if (!steam || !g_haveTag[ST_MOUSERAY])
	{
		AuditLine("[Audit] cursor: off (mouseRay probe not installed)");
		return;
	}
	HMODULE exe = (HMODULE)g_base;
	const unsigned char* seq = (const unsigned char*)(g_base + RVA_TRACE_CALLSEQ);
	bool seqOk = InModule(exe, seq, sizeof(TRACE_CALLSEQ_BYTES)) &&
	             memcmp(seq, TRACE_CALLSEQ_BYTES, sizeof(TRACE_CALLSEQ_BYTES)) == 0;
	bool maxOk = InModule(exe, (const void*)(g_base + RVA_FLT_MAX_CONST), 4) &&
	             *(const float*)(g_base + RVA_FLT_MAX_CONST) == FLT_MAX;
	g_cursor.layoutOk   = seqOk && maxOk;
	g_cursor.splitOn    = g_cfg.cursorSplitEvery > 0 && g_cursor.layoutOk;

	std::string split = g_cfg.cursorSplitEvery <= 0 ? std::string("off")
		: (g_cursor.splitOn ? Fmt("every %d cursor-ray frames", g_cfg.cursorSplitEvery)
		                    : std::string("OFF (traceAll layout mismatch)"));
	AuditLine(Fmt("[Audit] cursor: traceAll layout %s (callseq=%d fltmax=%d) hits=%s sort=%s ray2=%s charMask=0x%08X split=%s",
	              g_cursor.layoutOk ? "ok" : "MISMATCH", seqOk ? 1 : 0, maxOk ? 1 : 0,
	              g_cursor.layoutOk ? "on" : "off", g_haveTag[ST_CURSORT] ? "on" : "off",
	              g_haveTag[ST_MOUSERAY2] ? "on" : "off", g_cfg.cursorCharGroups, split.c_str()));
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

bool AuditHookExe(const char* name, size_t rva, const unsigned char* expect, void* detour, void** orig)
{
	return HookAt(name, (HMODULE)g_base, ExeAddr(rva), expect, detour, orig);
}

std::string AuditCheckExe(const char* name, size_t rva, const unsigned char* expect)
{
	if (NameDisabled(name))
		return "SKIP ini";
	return CheckPrologue((HMODULE)g_base, ExeAddr(rva), expect);
}

} // audit
