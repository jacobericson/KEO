// audit_save.cpp - Save stage timing detours.
// Main thread records save stages and queues reports under g_lineCS alone.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {
// =========================================================================
// Save / autosave pipeline ([Audit] SaveDetail)
// =========================================================================
//
// An autosave is one blocking main-thread frame. SaveManager::updateAutoSave
// runs every frame from GameWorld::mainLoop and only keeps a timer; on the
// frame the countdown expires it sets autoSaveTimer to -1 and paints
// "Autosaving...". The next frame arms SaveManager::save (signal=1, delay=1),
// and one frame after that SaveManager::execute -- already timed as the `save`
// call-site probe -- calls SaveManager::saveGame. That call is the stall: it
// serialises the world and writes every file inline on the main thread. Only
// the final temp-folder copy is asynchronous, on the SaveFileSystem thread;
// SaveFileSystem::sync reports its completion back on the main thread.
//
// Every stage hook returns immediately unless a saveGame is on the stack, so
// nothing outside a save pays for them. The game measures the same totals
// itself and throws them away: saveGame's own log call is a nullsub in the
// release build.

enum SaveStage
{
	SV_ZONESTATES = 0,  // ZoneManager::saveActiveZoneStates    (the whole Set B loop)
	SV_ZONELEVEL,       //   ZoneMapContent::saveLevelData      per zone, inclusive
	SV_ZONESER,         //     RootObjectContainer::serialiseThings
	SV_ZONEITEMS,       //     ZoneMapContent::saveItems
	SV_ZONEDISK,        //     ZoneMapContent::saveToDisk       inclusive of its write
	SV_GDCZONE,         //       GameDataContainer::save under a zone
	SV_GDCMAIN,         //   GameDataContainer::save straight from saveGame (quick.save, main)
	SV_GDCNEST,         //   GameDataContainer::save nested in another stage (towns, platoons)
	SV_TOWNSUNIQ,       // TownList::saveUniqueTownsAndNests
	SV_TOWNSTATE,       // TownList::saveState
	SV_FACPLAYER,       // FactionManager::savePlayerGameState
	SV_FACSTATE,        // FactionManager::saveGameState
	SV_PORTRAIT,        // PortraitManager::saveTexture (render-to-texture readback)
	SV_SFSHAND,         // SaveFileSystem::saveGame (queues the copy, starts the thread)
	SV_COUNT
};

const char* SAVE_STAGE_NAMES[SV_COUNT] =
{
	"zoneStates", "zoneLevel", "zoneSer", "zoneItems", "zoneDisk", "gdcZone",
	"gdcMain", "gdcNest", "townsUniq", "townsState", "facPlayer", "facState", "portrait",
	"sfsHandoff"
};

// Stages that sit directly under saveGame. What is left after subtracting them
// is the serialisation saveGame does inline: camera, money, player, the 64x64
// zone scan, the dialogue and GUI blocks.
const int SAVE_TOP_STAGES[] =
{
	SV_ZONESTATES, SV_GDCMAIN, SV_TOWNSUNIQ, SV_TOWNSTATE,
	SV_FACPLAYER, SV_FACSTATE, SV_PORTRAIT, SV_SFSHAND
};
const int SAVE_TOP_STAGE_COUNT = sizeof(SAVE_TOP_STAGES) / sizeof(SAVE_TOP_STAGES[0]);

const int SAVE_MAX_ZONE_ROWS = 96;

struct SaveZoneRow
{
	int      gx, gy;
	unsigned things;      // ZoneMapContent::things.count on entry
	LONGLONG ticks;       // saveLevelData, inclusive
};

struct SaveState
{
	int         depth;         // > 0 while SaveManager::saveGame is on the stack
	int         diskDepth;     // > 0 while ZoneMapContent::saveToDisk is on the stack
	int         levelDepth;    // > 0 while ZoneMapContent::saveLevelData is on the stack
	int         stageDepth;    // > 0 while any stage below saveGame is on the stack
	LONGLONG    t0;            // saveGame entry
	LONGLONG    ticks[SV_COUNT];
	int         calls[SV_COUNT];
	unsigned    things;        // summed over zones, before the dedup pass
	unsigned    thingsMax;
	int         autosave;      // SaveManager::flags at entry (1 = autosave)
	int         seq;           // saves this session
	SaveZoneRow rows[SAVE_MAX_ZONE_ROWS];
	int         rowCount;
	LONGLONG    armedAt;       // updateAutoSave set the timer to -1
	// Asynchronous stage, handed to the SaveFileSystem thread.
	LONGLONG    asyncT0;
	LONGLONG    asyncSaveT0;   // that save's saveGame entry, for wall=
	int         asyncSeq;      // save the copy in flight belongs to (0 = none)
	int         asyncMsgs;
	float       blockingMs;
};

SaveState g_save;
bool      g_saveHooked = false;

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

SmSaveGame_t   oSmSaveGame   = NULL;
SmUpdateAuto_t oSmUpdateAuto = NULL;
ZmSaveStates_t oZmSaveStates = NULL;
ZmcSaveLevel_t oZmcSaveLevel = NULL;
RocSerialise_t oRocSerialise = NULL;
ZmcSaveItems_t oZmcSaveItems = NULL;
ZmcSaveDisk_t  oZmcSaveDisk  = NULL;
GdcSave_t      oGdcSave      = NULL;
TlSave_t       oTlSaveUnique = NULL;
TlSave_t       oTlSaveState  = NULL;
FmSave_t       oFmSavePlayer = NULL;
FmSave_t       oFmSaveState  = NULL;
PmSaveTex_t    oPmSaveTex    = NULL;
SfsSaveGame_t  oSfsSaveGame  = NULL;
SfsSync_t      oSfsSync      = NULL;

// True only on the main thread with a saveGame on the stack. Every stage hook
// is a plain pass-through otherwise, so the level editor and the mod tools
// (which also reach GameDataContainer::save) are never timed.
inline bool SaveActive()
{
	return g_save.depth > 0 && IsMain();
}

inline void SaveAdd(int stage, LONGLONG ticks)
{
	g_save.ticks[stage] += ticks;
	++g_save.calls[stage];
}

void SaveReport()
{
	LONGLONG total = Now() - g_save.t0;
	g_save.blockingMs = TicksToMs(total);

	LONGLONG top = 0;
	for (int i = 0; i < SAVE_TOP_STAGE_COUNT; ++i)
		top += g_save.ticks[SAVE_TOP_STAGES[i]];
	// saveLevelData minus the three stages nested in it: the duplicate scan
	// over ZoneMapContent::things (every pair, two virtual calls each) plus
	// that function's own bookkeeping.
	LONGLONG dedup = g_save.ticks[SV_ZONELEVEL] - g_save.ticks[SV_ZONESER] -
	                 g_save.ticks[SV_ZONEITEMS] - g_save.ticks[SV_ZONEDISK];
	LONGLONG zoneLoop  = g_save.ticks[SV_ZONESTATES] - g_save.ticks[SV_ZONELEVEL];
	LONGLONG diskOther = g_save.ticks[SV_ZONEDISK] - g_save.ticks[SV_GDCZONE];

	float armMs = g_save.armedAt ? TicksToMs(g_save.t0 - g_save.armedAt) : -1.0f;
	AuditLine(Fmt("[AUDIT-SAVE] #%d kind=%s blocking=%.1fms zones=%d things=%u thingsMax=%u armToSave=%.0fms",
	              g_save.seq, g_save.autosave ? "autosave" : "manual",
	              g_save.blockingMs, g_save.calls[SV_ZONELEVEL],
	              g_save.things, g_save.thingsMax, armMs));

	std::string stages;
	for (int i = 0; i < SV_COUNT; ++i)
	{
		if (!g_save.calls[i])
			continue;
		stages += Fmt(" %s=%.1f/%d", SAVE_STAGE_NAMES[i],
		              TicksToMs(g_save.ticks[i]), g_save.calls[i]);
	}
	AuditLine(Fmt("[AUDIT-SAVE] #%d stages ms/calls%s", g_save.seq, stages.c_str()));

	AuditLine(Fmt("[AUDIT-SAVE] #%d derived zoneDedup=%.1f zoneLoop=%.1f diskOther=%.1f saveInline=%.1f",
	              g_save.seq, TicksToMs(dedup), TicksToMs(zoneLoop),
	              TicksToMs(diskOther), TicksToMs(total - top)));

	// Slowest zones first: per-zone cost should track things squared if the
	// duplicate scan dominates, and things linearly if the write does.
	int rows = g_save.rowCount;
	for (int i = 1; i < rows; ++i)
	{
		SaveZoneRow key = g_save.rows[i];
		int j = i - 1;
		while (j >= 0 && g_save.rows[j].ticks < key.ticks)
		{
			g_save.rows[j + 1] = g_save.rows[j];
			--j;
		}
		g_save.rows[j + 1] = key;
	}
	int show = rows < g_cfg.saveTop ? rows : g_cfg.saveTop;
	for (int i = 0; i < show; ++i)
	{
		const SaveZoneRow& r = g_save.rows[i];
		AuditLine(Fmt("[AUDIT-SAVE] #%d zone (%d,%d) things=%u ms=%.1f",
		              g_save.seq, r.gx, r.gy, r.things, TicksToMs(r.ticks)));
	}
}

int hk_SmSaveGame(void* sm, const void* location, const void* name)
{
	if (!g_cfg.saveDetail || !IsMain() || g_save.depth > 0)
		return oSmSaveGame(sm, location, name);

	memset(g_save.ticks, 0, sizeof(g_save.ticks));
	memset(g_save.calls, 0, sizeof(g_save.calls));
	g_save.rowCount  = 0;
	g_save.things    = 0;
	g_save.thingsMax = 0;
	g_save.diskDepth  = 0;
	g_save.levelDepth = 0;
	g_save.stageDepth = 0;
	g_save.autosave  = PlausiblePtr((uintptr_t)sm)
	                 ? *(const int*)((uintptr_t)sm + SM_FLAGS) : 0;
	++g_save.seq;
	g_save.t0 = Now();
	++g_save.depth;
	int r = oSmSaveGame(sm, location, name);
	--g_save.depth;
	SaveReport();
	g_save.armedAt = 0;
	return r;
}

void hk_SmUpdateAuto(void* sm)
{
	if (!g_cfg.saveDetail || !IsMain() || !PlausiblePtr((uintptr_t)sm))
	{
		oSmUpdateAuto(sm);
		return;
	}
	float before = *(const float*)((uintptr_t)sm + SM_AUTOSAVE_TIMER);
	oSmUpdateAuto(sm);
	float after = *(const float*)((uintptr_t)sm + SM_AUTOSAVE_TIMER);
	// -1 is reached only on the frame the countdown expires: the frame that
	// paints "Autosaving..." and is followed by SaveManager::save.
	if (after == -1.0f && before != -1.0f)
	{
		uintptr_t opt = KlibAddress(g_base, RVA_OPTIONS);
		g_save.armedAt = Now();
		AuditLine(Fmt("[SAVE] autosave armed interval=%.1fmin waited=%.0fs",
		              *(const float*)(opt + OPT_AUTOSAVE_TIME), before));
	}
}

void hk_ZmSaveStates(void* zm)
{
	if (!SaveActive())
	{
		oZmSaveStates(zm);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oZmSaveStates(zm);
	--g_save.stageDepth;
	SaveAdd(SV_ZONESTATES, Now() - t0);
}

void hk_ZmcSaveLevel(void* zmc, bool all, const void* modName, const void* pathOverride)
{
	if (!SaveActive())
	{
		oZmcSaveLevel(zmc, all, modName, pathOverride);
		return;
	}
	unsigned things = 0;
	int gx = -1, gy = -1;
	if (PlausiblePtr((uintptr_t)zmc))
	{
		things = *(const unsigned*)(KLIB_MEMBER(5, (uintptr_t)zmc,
		            RootObjectContainer_things_count, ROC_THINGS_COUNT));
		uintptr_t zone = *(const uintptr_t*)((uintptr_t)zmc + ZMC_ZONEMAP);
		if (PlausiblePtr(zone))
		{
			gx = *(const int*)(KLIB_MEMBER(5, zone, ZoneMap_coordinates_x, ZONE_COORD_X));
			gy = *(const int*)(KLIB_MEMBER(5, zone, ZoneMap_coordinates_y, ZONE_COORD_Y));
		}
	}
	LONGLONG t0 = Now();
	++g_save.levelDepth;
	oZmcSaveLevel(zmc, all, modName, pathOverride);
	--g_save.levelDepth;
	LONGLONG d = Now() - t0;
	SaveAdd(SV_ZONELEVEL, d);
	g_save.things += things;
	if (things > g_save.thingsMax)
		g_save.thingsMax = things;
	if (g_save.rowCount < SAVE_MAX_ZONE_ROWS)
	{
		SaveZoneRow& r = g_save.rows[g_save.rowCount++];
		r.gx = gx; r.gy = gy; r.things = things; r.ticks = d;
	}
}

void hk_RocSerialise(void* roc, const void* things, void* out, void* source,
                     void* offset, const void* mod)
{
	if (!SaveActive())
	{
		oRocSerialise(roc, things, out, source, offset, mod);
		return;
	}
	LONGLONG t0 = Now();
	oRocSerialise(roc, things, out, source, offset, mod);
	if (g_save.levelDepth > 0)
		SaveAdd(SV_ZONESER, Now() - t0);
}

void hk_ZmcSaveItems(void* zmc, void* datas, bool all)
{
	if (!SaveActive())
	{
		oZmcSaveItems(zmc, datas, all);
		return;
	}
	LONGLONG t0 = Now();
	oZmcSaveItems(zmc, datas, all);
	SaveAdd(SV_ZONEITEMS, Now() - t0);
}

void hk_ZmcSaveDisk(void* zmc, const void* path)
{
	if (!SaveActive())
	{
		oZmcSaveDisk(zmc, path);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.diskDepth;
	oZmcSaveDisk(zmc, path);
	--g_save.diskDepth;
	SaveAdd(SV_ZONEDISK, Now() - t0);
}

bool hk_GdcSave(void* gdc, const void* filename, void* moreData)
{
	if (!SaveActive())
		return oGdcSave(gdc, filename, moreData);
	LONGLONG t0 = Now();
	bool r = oGdcSave(gdc, filename, moreData);
	int bucket = g_save.diskDepth > 0 ? SV_GDCZONE
	           : (g_save.stageDepth > 0 ? SV_GDCNEST : SV_GDCMAIN);
	SaveAdd(bucket, Now() - t0);
	return r;
}

void hk_TlSaveUnique(void* tl, void* datas)
{
	if (!SaveActive())
	{
		oTlSaveUnique(tl, datas);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oTlSaveUnique(tl, datas);
	--g_save.stageDepth;
	SaveAdd(SV_TOWNSUNIQ, Now() - t0);
}

void hk_TlSaveState(void* tl, void* datas)
{
	if (!SaveActive())
	{
		oTlSaveState(tl, datas);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oTlSaveState(tl, datas);
	--g_save.stageDepth;
	SaveAdd(SV_TOWNSTATE, Now() - t0);
}

void hk_FmSavePlayer(void* fm, void* datas)
{
	if (!SaveActive())
	{
		oFmSavePlayer(fm, datas);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oFmSavePlayer(fm, datas);
	--g_save.stageDepth;
	SaveAdd(SV_FACPLAYER, Now() - t0);
}

void hk_FmSaveState(void* fm, void* datas)
{
	if (!SaveActive())
	{
		oFmSaveState(fm, datas);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oFmSaveState(fm, datas);
	--g_save.stageDepth;
	SaveAdd(SV_FACSTATE, Now() - t0);
}

void hk_PmSaveTex(void* pm)
{
	if (!SaveActive())
	{
		oPmSaveTex(pm);
		return;
	}
	LONGLONG t0 = Now();
	++g_save.stageDepth;
	oPmSaveTex(pm);
	--g_save.stageDepth;
	SaveAdd(SV_PORTRAIT, Now() - t0);
}

char hk_SfsSaveGame(void* sfs, const void* savePath)
{
	if (!g_cfg.saveDetail || !IsMain())
		return oSfsSaveGame(sfs, savePath);
	LONGLONG t0 = Now();
	char r = oSfsSaveGame(sfs, savePath);
	LONGLONG d = Now() - t0;
	if (g_save.depth > 0)
		SaveAdd(SV_SFSHAND, d);
	// r != 0: the state is SAVING and the copy thread has been started.
	if (r && PlausiblePtr((uintptr_t)sfs))
	{
		g_save.asyncT0     = Now();
		g_save.asyncSaveT0 = g_save.t0;
		g_save.asyncSeq    = g_save.seq;
		g_save.asyncMsgs   = *(const int*)((uintptr_t)sfs + SFS_MSGS_COUNT);
	}
	return r;
}

void hk_SfsSync(void* sfs)
{
	if (!g_cfg.saveDetail || !IsMain() || !PlausiblePtr((uintptr_t)sfs))
	{
		oSfsSync(sfs);
		return;
	}
	int before = *(const int*)((uintptr_t)sfs + SFS_STATE);
	oSfsSync(sfs);
	// sync folds a COMPLETE copy back in and clears the state to idle.
	if (before == 2 && g_save.asyncSeq)
	{
		AuditLine(Fmt("[AUDIT-SAVE] #%d async copy=%.0fms msgs=%d wall=%.0fms blocking=%.1fms",
		              g_save.asyncSeq, TicksToMs(Now() - g_save.asyncT0),
		              g_save.asyncMsgs, TicksToMs(Now() - g_save.asyncSaveT0),
		              g_save.blockingMs));
		g_save.asyncSeq = 0;
	}
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
