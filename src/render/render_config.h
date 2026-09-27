#pragma once
#include "base/ini_text.h"
#include <string>
#include <vector>

struct RenderConfig
{
	bool  renderLevers;              // master switch
	bool  reflectionHalfRate;
	bool  shadowReachDiag;           // DEV: count shadow casters that can reach a receiving pixel
	bool  particleOffscreenSkip;
	bool  particleStepCap;
	bool  gpuParamLookupDiag;
	bool  renderDiag;                // the 30 s Render: line
	float particleStepCapSpeed;      // game speed above which the cap applies
	float particleOffscreenSeconds;  // non-visible timeout handed to ParticleUniverse
	float particleOffscreenMinAge;   // effect age (game seconds) before the timeout is set
	char  particleLoopingNames[256]; // comma list of template-name substrings
	bool  oldAnimSkip;               // skip the old-animation fork when nothing would update
	bool  oldAnimDiag;               // DEV: count the passes oldAnimSkip could skip
	bool  gpuParamCache;             // cache shader constant definitions by map and name
	bool  shadowReachCull;           // drop shadow casters that cannot reach a receiving pixel
	bool  emptyPassSkip;             // Debug and InteriorMask compositor nodes off while they would draw nothing
	float foliagePageBudgetMs;       // foliage page-build time per frame at speed; 0 = off
	float foliageBudgetSpeed;        // game speed above which the budget applies
	bool  gpuUploadDiag;             // DEV: count constant-buffer uploads identical to the last one
	bool  gpuUploadSkip;             // skip constant-buffer uploads identical to what the buffer holds
	bool  adoptOgrePurgeSkip;        // skip the per-cycle Ogre resource purge in an adoption cycle
	float adoptOgrePurgeMaxSkipSeconds;  // force a run after this long skipped; <= 0 never forces one
};

extern RenderConfig g_renderCfg;

// The compiled-in values (DEV and PROD differ), before the INI.
const RenderConfig& RenderConfigDefaults();

// Startup (LoadConfig): clamps the loaded render values, one line per change,
// then logs them all on one line.
void ClampRenderConfig();
void LogRenderConfig();

// Main thread, at runtime (the settings panel): clamps next, then logs and
// stores each key that differs from the live config, one field at a time.
// renderLevers is startup-only and never applied. A lever switched off is
// restored to the game's own state on the next render tick. True when
// anything was applied.
bool ApplyRenderConfig(const RenderConfig& next);
// Main thread: writes desired's render keys (clamped) plus extra (any other
// keys, e.g. startup-only ones) into KenshiZoneOpt.ini next to the DLL in
// one atomic replace; every other line stays as it is. A render key the file
// lacks is added only when it differs from the compiled-in default. False
// (and a log line) when the file could not be read or replaced.
bool SaveRenderConfig(const RenderConfig& desired,
                      const std::vector<IniEntry>& extra = std::vector<IniEntry>());

// Main thread: writes entries into KenshiZoneOpt.ini in one atomic replace.
// section is a bracketed header ("[Bench]") that append entries with no
// existing line land inside, added first if the file lacks it; NULL keeps
// RewriteIniKeys' default placement (before the first section header).
// Log lines start "<logPrefix>: ". False (and a log line) when the file could
// not be read or replaced.
bool SaveIniEntries(const std::vector<IniEntry>& entries, const char* section, const char* logPrefix);
