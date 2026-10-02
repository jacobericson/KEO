// plugin_entry.cpp - startPlugin and its startup phases. Main thread.
#include "plugin/plugin_entry_internal.h"
#include <cstdio>     // _snprintf_s (module-base log line)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "zone/preload/preload.h"
#include "zone/reset/zone_reset_gate.h"
#include "navmesh/nm_workers.h"
#include "navmesh/generation/nm_misspar.h"
#include "navmesh/jobs/nm_buildlock.h"
#include "plugin/hook_manifest.h"
#include "render/render_config.h"
#include "render/render_levers.h"
#include "gui/settings_panel.h"
#include "bench/bench_lever_ab.h"
#include "bench/bench_runner.h"
#include "bench/bench_sweep.h"
// GetKenshiVersion. The library's headers raise C4482, C4005 and C4099,
// warnings about KenshiLib's code, not ours; the include pair silences them
// around this include only.
#include "base/klib_include.h"
#include <kenshi/Kenshi.h>
#include "base/klib_include_end.h"
#include "movement/tracking.h"
#include "navmesh/scheduling/navmesh_sched.h"
#include "fixes/physx/purecall_record.h"
#include "fixes/crash_claim.h"
#include "diag/mem_probe.h"
#include "diag/fatal_class.h"
#include "diag/cpp_exception.h"
#include "diag/throw_ring.h"
#include "fixes/streaming/navmesh_update_guard.h"
#include "diag/module_bases.h"
#include "diag/exit_capture.h"
#include "fixes/physx/physx_query_guard.h"
#include "fixes/stitch/stitch_byte_guard.h"
#include "fixes/streaming/section_key_ring.h"
#include "plugin/crash_record.h"

// InitLogFile is declared in core.h


// =========================================================================
// DLL entry point — graceful worker shutdown on unload
// =========================================================================

// Vectored-handler registration: startPlugin registers the handler and
// DllMain unregisters it on process detach.
static PVOID g_vehHandle = NULL;


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH && g_vehHandle)
	{
		// Unregister before the code behind the handler can go away.
		RemoveVectoredExceptionHandler(g_vehHandle);
		g_vehHandle = NULL;
	}
	if (reason == DLL_PROCESS_DETACH)
	{
		// Same reasoning as the VEH unregister above, for a pointer that lives
		// in a different module: if this DLL unloads while PhysXCore64 (and
		// other threads) can still run, a stale handler pointer into our own
		// freed code would turn a diagnosable abort into a crash into nothing.
		UninstallPurecallRecorder();

		// The patched bytes stay; only the pointer the stub calls through is
		// repointed, into the stub's own page. See NeutralizePhysQueryGuard.
		NeutralizePhysQueryGuard();
		NeutralizeStitchByteGuard();
	}
	if (reason == DLL_PROCESS_DETACH)
	{
		// Signal only, never wait. The bounded join belongs in the NavMesh::stop
		// hook, which runs on a game thread well before process detach; waiting
		// here would block under the loader lock. The event is manual-reset,
		// so one set releases every worker and stays signalled.
		g_workerShutdown = 1;
		if (g_jobEvent)
			SetEvent(g_jobEvent);

		if (g_jobEvent)
		{
			CloseHandle(g_jobEvent);
			g_jobEvent = NULL;
		}
	}
	return TRUE;
}




namespace plugin_entry_detail
{
struct EntryCtx
{
	std::string dllDir;
	unsigned long crashClaimError;
	CrashClaimOutcome crashClaimOutcome;
	std::string gateToken;
	bool gateOk;
	int renderInstalled;
	int renderWanted;
	std::string benchToken;
};

// Main thread: resolve crash paths, pin the module and arm both handlers before
// logging can fault. No mod lock is held across this phase.
void PrepareProcess(EntryCtx& ctx)
{
	gameBase = (uintptr_t)GetModuleHandleA(NULL);

	// As early as possible, so a fault anywhere else in startup still has a
	// module table behind it. Toolhelp is safe here; never call it again
	// from the crash path itself (see SnapshotModuleBases's own comment).
	SnapshotModuleBases();

	// Pin our module. A vectored handler stays registered process-wide; if
	// this DLL were ever unloaded with the registration live, the next fault
	// would jump into freed memory. Pinning makes the unload impossible.
	{
		HMODULE self = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			(LPCSTR)&NavMeshCrashHandler, &self);
	}

	// Resolve the crash file path here, on the main thread, so the handler
	// never touches the loader in a fault context. Claim any leftover file
	// first, before g_crashFilePath exists to point anything at that name.
	ctx.dllDir = GetDLLDirectory();
	ctx.crashClaimError = 0;
	ctx.crashClaimOutcome = ClaimPreviousCrashDump(ctx.dllDir, &ctx.crashClaimError);
	{
		std::string crashPath = CrashDumpCurrentPath(ctx.dllDir);
		if (crashPath.size() < sizeof(g_crashFilePath))
			memcpy(g_crashFilePath, crashPath.c_str(), crashPath.size() + 1);

		// Deleted rather than claimed: this file is only ever written by the
		// handler below, so removing any leftover makes its later presence
		// mean "this session threw", with no mtime guesswork.
		std::string cppExPath = CppExceptionDumpPath(ctx.dllDir);
		DeleteFileA(cppExPath.c_str());
		if (cppExPath.size() < sizeof(g_cppExFilePath))
			memcpy(g_cppExFilePath, cppExPath.c_str(), cppExPath.size() + 1);
	}
	ProfilerImageResolve();

	// Before the handlers, so the first fault of the process already has a
	// memory figure to print and its live read has its entry points resolved.
	MemProbeInit();

	// Also before the handlers: the C++-throw path times itself with
	// ElapsedSec, and an unset frequency would make that clock infinite and
	// the spacing rule below it meaningless for the rest of the process.
	QueryPerformanceFrequency(&qpcFrequency);
	QueryPerformanceCounter(&pluginStartTime);

	g_vehHandle = AddVectoredExceptionHandler(1, NavMeshCrashHandler);
	// Not undone at process detach: the module is pinned above, so the code
	// behind the filter cannot go away, and the game's own shutdown puts the
	// original filter back on its way out.
	g_prevUnhandledFilter = SetUnhandledExceptionFilter(KEOUnhandledFilter);
}

// Main thread: open the log and load configuration after the crash handlers are
// armed; initialise the update guard before hooks can reach it. Logs take logCS.
void InitializeLogAndConfig(const EntryCtx& ctx)
{
	InitLogFile();
	LogMsg(CrashClaimLogLine(ctx.crashClaimOutcome, ctx.crashClaimError));
	LoadConfig(ctx.dllDir);
	SetCoreGameBase(gameBase);
	// Before any hook is installed, so the navmesh/path thread can never reach
	// the wrapper while the guard is half-initialised.
	NavMeshUpdateGuardInit(ctx.dllDir, (unsigned __int64)gameBase, fixes::g_fixesCfg.navMeshUpdateGuardEnabled);
	// Armed-but-never-fired must not read the same as switched off.
	LogMsg(fixes::g_fixesCfg.navMeshUpdateGuardEnabled ? "navMeshUpdateGuard: armed" : "navMeshUpdateGuard: off");

	// DEV only: names who calls TerminateProcess when the game vanishes with
	// no dialog, no crash_dump.txt and no WER dump. A no-op outside
	// KEO_DEBUG. Kernel32 and ntdll are always mapped this early,
	// so unlike PurecallRecord's PhysXCore64 wait, there is nothing to defer.
	InstallExitCapture(ctx.dllDir);

	// A human triaging a crash record for a crash address inside RE_Kenshi.dll
	// computes addr - RE_Kenshi base from this line.
	{
		HMODULE reKenshi = GetModuleHandleA("RE_Kenshi.dll");
		HMODULE self = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&NavMeshCrashHandler, &self);
		char line[160];
		if (reKenshi)
			_snprintf_s(line, sizeof(line), _TRUNCATE,
				"Module bases: exe=0x%llX RE_Kenshi.dll=0x%llX KEO.dll=0x%llX",
				(unsigned long long)gameBase, (unsigned long long)(uintptr_t)reKenshi,
				(unsigned long long)(uintptr_t)self);
		else
			_snprintf_s(line, sizeof(line), _TRUNCATE,
				"Module bases: exe=0x%llX RE_Kenshi.dll=not-loaded KEO.dll=0x%llX",
				(unsigned long long)gameBase, (unsigned long long)(uintptr_t)self);
		LogMsg(line);
	}

	// The full table a crash record itself carries (see EmitCrashRecord),
	// printed once here too so a normal session's log can be checked
	// against a crash record's addresses without waiting for one to happen.
	{
		char modLine[2048];
		int modTrunc = 0;
		FormatModuleBases(modLine, sizeof(modLine), g_moduleBases, g_moduleBaseCount, &modTrunc);
		std::ostringstream full;
		full << modLine;
		if (modTrunc > 0)
			full << " modTrunc=" << modTrunc;
		LogMsg(full.str());
	}
}

// Main thread: verify version, bindings and prologues before feature installs.
// Refusal disables the flags and writes its banner under logCS before returning.
bool CheckBuildGate(EntryCtx& ctx)
{
	// ---------------------------------------------------------------
	// Build gate: nothing is installed on a binary we don't know
	// ---------------------------------------------------------------
	//
	// Two checks, in order. The version tells us whether KenshiLib's tables and
	// ours are talking about the same executable; the prologue pass proves it
	// site by site. A mod that writes 5-byte jumps into 20 hardcoded addresses
	// has to do this: on a mismatched build the jumps land mid-instruction and
	// the game dies somewhere unrelated, hours later.
	ctx.gateToken = "FAILED";
	ctx.gateOk = true;
	bool gogUnsupported = false;
	{
		KenshiLib::BinaryVersion ver = KenshiLib::GetKenshiVersion();
		KenshiLib::BinaryVersion::KenshiPlatform plat = ver.GetPlatform();
		bool versionOk = (ver.GetVersion() == "1.0.65")
			&& (plat == KenshiLib::BinaryVersion::STEAM
			 || plat == KenshiLib::BinaryVersion::GOG);

		LogMsg("Kenshi build: " + ver.ToString() +
			(versionOk ? " (supported)" : " (UNSUPPORTED)"));

		if (!versionOk)
			ctx.gateOk = false;
		else if (plat == KenshiLib::BinaryVersion::GOG)
		{
			// GOG 1.0.65 passes the version check above (it's a supported
			// KenshiLib build), but g_hookPrologues is Steam's table: every
			// row would mismatch and the refusal below would read as
			// corruption ("20/20 prologues mismatched") instead of naming the
			// real reason. Say the real reason and skip the (pointless, all-
			// mismatch) prologue pass entirely.
			LogMsg("Build gate: the prologue table is Steam's; GOG "
			       "1.0.65 is not supported until KenshiLib covers every site (B2)");
			gogUnsupported = true;
			ctx.gateOk = false;
		}
	}

	// Address mismatch is fatal in DEV and PROD, before any prologue or hook.
	if (ctx.gateOk && !InitKlibBindings(gameBase, LogKlibBinding))
		ctx.gateOk = false;

	int gateBad = 0;
	int gateShared = 0;
	int gateDiagBad = 0;
	std::ostringstream gateDiagNames;
	if (ctx.gateOk)
	{
		for (int i = 0; i < g_hookPrologueCount; ++i)
		{
			bool shared = false;
			bool passed = VerifyPrologue(g_hookPrologues[i].rva,
			                             g_hookPrologues[i].bytes,
			                             g_hookPrologues[i].name,
			                             &shared);
			HookRowNoteGateVerdict((HookRowId)i, passed);
			if (!passed)
			{
				// A diagnostic site is not worth refusing to install over: turn that
				// one diagnostic off and carry on. Everything else is fatal.
				if (g_hookPrologues[i].diagnostic)
				{
					if (gateDiagBad > 0)
						gateDiagNames << ", ";
					gateDiagNames << g_hookPrologues[i].name;
					gateDiagBad++;
				}
				else
					gateBad++;
			}
			else if (shared)
				gateShared++;
		}
		if (gateBad > 0)
			ctx.gateOk = false;
		if (gateDiagBad > 0)
		{
			// A row installed without a re-verify is patched only if the gate passed it: HookInstallRow reads this verdict.
			std::ostringstream ds;
			ds << "Build gate: " << gateDiagBad
			   << " diagnostic site(s) mismatched (" << gateDiagNames.str()
			   << ") — each refuses its own install, the rest of the plugin "
			      "installs normally";
			LogMsg(ds.str());
		}
	}

	if (!ctx.gateOk)
	{
		std::ostringstream gs;
		if (gogUnsupported)
			gs << "Build gate failed: GOG platform not supported yet (B2)";
		else
			gs << "Build gate failed: version/address/prologue refusal; installing nothing ("
			   << gateBad << "/" << g_hookPrologueCount << " prologues mismatched)";
		LogMsg(gs.str());

		// Every feature off, so anything that reads a flag later (and the
		// banner below) sees a plugin that does nothing at all.
		zone::g_zoneCfg.deferralEnabled      = false;
		navmesh::g_navmeshCfg.priorityBoostEnabled = false;
		zone::g_zoneCfg.preloadEnabled       = false;
		zone::g_zoneCfg.movementAwareEnabled = false;
		navmesh::g_navmeshCfg.cachingEnabled       = false;
		movement::g_movementCfg.islandFixEnabled     = false;
		movement::g_movementCfg.cfg_islandFarSpan    = 0;
		movement::g_movementCfg.groupCohesionEnabled = false;
		pathfind::g_pathfindCfg.pathfindDiagEnabled   = false;
		LogInitBanner(0, 0, "FAILED", 0, 0, "off(gate)");
		return false;
	}

	{
		// gateToken keeps the banner's grepped "gate=ok(...)" token exactly as
		// it was; only the standalone log line's wording changes (it used to
		// read "Build gate: ok, ok(...)", with "ok" twice).
		std::ostringstream detail;
		detail << g_hookPrologueCount << " sites, " << gateShared << " shared";
		ctx.gateToken = "ok(" + detail.str() + ")";
		LogMsg("Build gate: ok (" + detail.str() + ")");
	}
	return true;
}

// Main thread: record layout after the gate accepts the executable and before
// game bindings or runtime state are installed. Logging takes logCS.
void LogStartupLayout()
{
	// Startup layout record: the sizes of WatchedCharacter and SchedMoverInfo.
	// The "(PATHFIND_STEP=4)" suffix stays only to keep the line's format stable.
	{
		std::ostringstream ss;
		ss << "sizeof(WatchedCharacter)=" << sizeof(WatchedCharacter)
		   << " sizeof(SchedMoverInfo)=" << sizeof(SchedMoverInfo)
		   << " (PATHFIND_STEP=" << 4 << ")";
		LogMsg(ss.str());
	}
}

// Main thread: install bindings, render levers, benchmark and settings first,
// then initialise worker state and locks before hooks can dispatch any job.
// No mod lock is held across this phase; logging takes logCS inside its calls.
void InitializeRuntimeState(EntryCtx& ctx)
{
	// Initialize all function pointers from game RVAs
	InitGameBindings(gameBase);

	ctx.renderInstalled = 0;
	ctx.renderWanted = 0;
	if (g_renderCfg.renderLevers)
		InstallRenderLevers(&ctx.renderInstalled, &ctx.renderWanted);
	ctx.benchToken = BenchRunnerInstall();
	BenchRegisterScenario("leverAB", BuildLeverAB);   // kind 0, every slot's
	BenchSweepRunner sweepRunner = { &BenchRunnerArm, &BenchRunnerActive, &BenchRunnerAbort, &BenchRunnerArmBlocked, &LogMsg };
	BenchSweepSetRunner(sweepRunner);
	BenchRunnerSetEndCallback(&BenchSweepOnRunEnd);
	InstallSettingsPanel();


	// Manual reset: the event means "the queue may be non-empty", not
	// "one job is waiting". Every observer of a non-empty queue sets it and only
	// a worker that finds the queue empty under the queue lock clears it, so a
	// burst of jobs wakes every idle worker instead of exactly one. DllMain's
	// shutdown SetEvent then wakes all of them and stays signalled.
	g_jobEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	InitNavMeshCacheCS();
	// processJobCS exists from here on; the save-load reset hook may take it.
	NavMeshMarkProcessJobLockReady();
	// The reset gate's event, before the reset hook and any NavMesh thread exist.
	// Without it a job parked by a reset polls the gate instead of waiting on it.
	if (!ZoneResetGateInit(&g_zoneResetGate))
	{
		DWORD gle = GetLastError();
		std::ostringstream rg;
		rg << "Save-load reset gate: event creation FAILED (gle=" << gle
		   << "); parked NavMesh jobs poll the gate instead";
		LogMsg(rg.str());
	}
	// The generation slots and the in-flight key table, before any dispatch.
	MissParInit();

	ClearPreloadState();
}

// Main thread: install hooks, emit their banner, then arm the three guards in
// order before navmesh threads exist. Logging takes logCS inside its calls.
void InstallHooksAndGuards(const EntryCtx& ctx)
{
	int installed = 0;
	int totalHooks = 0;
	InstallHooks(&installed, &totalHooks);

	LogInitBanner(installed, totalHooks, ctx.gateToken, ctx.renderInstalled, ctx.renderWanted, ctx.benchToken);

	// Not a hook: a data write into PhysXCore64.dll's own CRT, byte-checked
	// independently of the exe build gate above (it targets a different
	// module). Kept off the gate banner and off g_hookPrologueCount on
	// purpose -- it neither counts against nor depends on the 20-odd sites
	// that gate does track. PhysXCore64.dll is usually not loaded yet here;
	// this first attempt just covers the case where it already is, and
	// PurecallRecordTick (hook_updateCameraZone) retries the rest.
	InstallPurecallRecorder(ctx.gateOk && fixes::g_fixesCfg.physPurecallRecordEnabled);

	// Not a gate row either, and for a sharper reason: the site is six bytes
	// in the middle of a function body, and the gate's byte check accepts a
	// site another plugin detoured first. Mid-function that allowance is
	// wrong, so this verifies its own three addresses exactly and fails
	// closed. It installs no hook, so it moves neither gate=ok(N sites) nor
	// the hook count in the banner.
	InstallPhysQueryGuard(ctx.gateOk && fixes::g_fixesCfg.physQueryGuardEnabled);

	// The same kind of mid-function patch, off the gate banner for the same
	// reason. Its eight bytes cannot be written atomically, so it must arm
	// here, before the navmesh and its path thread exist.
	InstallStitchByteGuard(ctx.gateOk);

	// Worker threads created lazily on first hook_dispatchJob call
	// (Havok world not yet initialized at plugin load time)
}

}
using namespace plugin_entry_detail;

__declspec(dllexport) void startPlugin()
{
	EntryCtx ctx;
	PrepareProcess(ctx);
	InitializeLogAndConfig(ctx);
	if (!CheckBuildGate(ctx))
		return;
	LogStartupLayout();
	InitializeRuntimeState(ctx);
	InstallHooksAndGuards(ctx);
}
