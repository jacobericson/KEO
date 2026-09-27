#pragma once
#include <windows.h>
#include <string>

struct RenderStats
{
	volatile LONG     reflKept, reflSkipped;
	volatile LONG     reflFog;        // fog updates made for skipped reflections
	volatile LONG     fxTimeoutSet, fxTimeoutCleared, fxStepCapped;
	volatile LONG     fxRecordFull;   // timeouts not set because the record was full
	volatile LONG     fxTimedOut;     // state: systems carrying a timeout the lever set
	volatile LONG     lookupCalls;       // gpuParamLookupDiag: every lookup, timed
	volatile LONGLONG lookupTicks;
	volatile LONG     lookupHits, lookupMisses;   // gpuParamCache, main thread only
	volatile LONG     lookupOffThread;   // lookups off the main thread, passed through uncached
};
extern RenderStats g_renderStats;

// Installs every lever (DEV-only diagnostics in DEV builds only), whatever
// its key says. installed/wanted feed the banner token.
void InstallRenderLevers(int* installed, int* wanted);
// Why key's lever cannot act ("<sites> was refused at startup"); empty when
// it was installed or key has no lever.
std::string RenderLeverInertReason(const char* key);
// Main thread, once per frame from the camera-zone hook. saveLoading is the
// zone manager's just-loaded-a-game state for this frame.
void RenderLeversMainThreadTick(bool saveLoading);
// Main thread, at the end of each 30 s window: zeroes every counter and
// returns the stats line, empty when renderDiag is off.
std::string RenderLeverStatsLine();

// Lever installs, one per file; false leaves the lever inert.
bool InstallReflectionLever();
// Idempotent: resolves the ParticleUniverse exports and checks their layout once.
bool InstallParticleLevers();
// Idempotent: InstallParticleLevers() plus the ParticleSystem::_update hook.
bool InstallParticleStepCap();
// The shadow cascade hooks (cascade-setup arm, OgreMain cull filter and
// render-phase disarm) install once, shared by the two levers below; each
// install enables that lever's use of them.
// shadowReachDiag: the per-cascade reach counts and the depth dry run.
bool InstallShadowReach();
// shadowReachCull: drops the casters that cannot reach a receiver.
bool InstallShadowReachCull();
// Patches the D3D11 render system's import slot for GpuProgramParameters::getConstantDefinition.
bool InstallGpuParamsDiag();
// The same patch plus the two OgreMain hooks that invalidate the cache.
bool InstallGpuParamCache();
// Idempotent: the two OgreMain hooks alone (GpuNamedConstants' deletion and
// GpuProgramParameters::_setNamedConstants).
bool InstallGpuNamedConstantsWatch();
// Any thread: how many GpuNamedConstants have been deleted, each counted
// before it is freed.
LONG GpuNamedConstantsDeleted();
// Whether lookups currently go through the cache.
bool GpuParamCacheActive();

// Main thread, from RenderLeversMainThreadTick: every 60th pass, walks the
// active effects, rebuilds the looping table and applies the off-screen timeout.
// While a save loads it only publishes an empty table.
void ParticleLevers_MainThreadTick(bool saveLoading);
// Any thread, lock-free: whether sys was classified looping on the last walk.
bool ParticleSystemIsLooping(void* sys);

// Main thread, from RenderLeversMainThreadTick: disarms the cascade slot
// and clears the diagnostic's state when shadowReachDiag is switched.
void ShadowReach_MainThreadTick();
// Main thread: the Render: line's reachCut= field, and with the diagnostic
// on its reach=, sun= and depth fields, for the window just ended, per
// second. Always resets the counters.
std::string ShadowReachStatsToken(double windowSec);

// Main thread, from RenderLeversMainThreadTick: zeroes the lookup counters
// when gpuParamLookupDiag is switched off, empties the cache when
// gpuParamCache is.
void GpuParams_MainThreadTick();

// Idempotent: the SceneManager::updateAllOldAnimations hook, shared by
// oldAnimSkip and oldAnimDiag.
bool InstallOldAnimLever();
// Main thread, from RenderLeversMainThreadTick: zeroes the counters when the
// mode (off, oldAnimSkip, oldAnimDiag) is switched.
void OldAnim_MainThreadTick();
// Main thread: the Render: line's oldAnim= (and, with the diagnostic, scan=)
// fields for the window just ended, per loop. Always resets the counters;
// empty when both keys are off.
std::string OldAnimStatsToken(LONG loops);
// Resolves the OgreMain compositor exports and checks the layout the
// empty-pass listener reads.
bool InstallEmptyPassSkip();
// Main thread, from RenderLeversMainThreadTick: attaches the empty-pass
// listener to the current main workspace while emptyPassSkip is on; when it
// is off, switches the nodes back on and detaches it.
void EmptyPass_MainThreadTick();
// Main thread: the Render: line's empty-pass fields for the window just
// ended. Always resets the counters; empty when the lever is off.
std::string EmptyPassStatsToken();

// The PagedGeometry::update hook behind foliagePageBudgetMs.
bool InstallFoliageBudget();
// Main thread, from RenderLeversMainThreadTick, after the frame's foliage
// updates: closes the frame's budget.
void Foliage_MainThreadTick();
// Main thread: the Render: line's foliage=<calls>/<skipped>/<msMax> field for
// the window just ended (msMax: the largest frame total of timed calls).
// Always resets the counters; empty when foliagePageBudgetMs is 0.
std::string FoliageStatsToken();

// Idempotent: the D3D11 constant-buffer upload hook, shared by
// gpuUploadDiag and gpuUploadSkip.
bool InstallGpuUploadDiag();
// The upload hook plus the D3D11 buffer destructor hook and the
// GpuNamedConstants hooks the skip needs.
bool InstallGpuUploadSkip();
// Main thread, from RenderLeversMainThreadTick: frees every per-buffer copy
// and plan and zeroes the counters when both keys are switched off.
void GpuUpload_MainThreadTick();
// Main thread: the Render: line's upload= (gpuUploadDiag) and uploadSkip=
// and uploadPlan= (gpuUploadSkip) fields for the window just ended. Always resets the counters;
// empty while both keys are off.
std::string GpuUploadStatsToken(LONG loops);

// The Ogre::ResourceGroupManager and ResourceManager unload hooks behind
// adoptOgrePurgeSkip.
bool InstallOgrePurgeSkip();
// The Render: line's ogrePurge=<calls>/<skipped>/<fallback> field, calls
// intercepted since the last window regardless of key or eligibility. Always
// resets the counters; empty until the hooks are installed.
std::string OgrePurgeStatsToken();
