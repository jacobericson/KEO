// fixes_config.cpp - defaults and INI rows for the fixes module.
#include "fixes/fixes_config.h"
#include "base/config_rows.h"
#include "base/ini_text.h"
#include <cstddef>
#include <string.h>

// graphHeuristicGuard: a boolean stored as 1 or 0; anything else is refused.
static bool ParseGraphHeuristicGuard(const std::string& val, ConfigLogFn log)
{
	(void)log;
	bool b;
	if (!ParseBool(val, &b))
		return false;
	fixes::g_fixesCfg.graphHeuristicGuardOn = b ? 1 : 0;
	return true;
}

namespace fixes {

const FixesConfig kFixesDefaults =
{
	true, // pathExtractGuardEnabled
	true, // sectionStampEnabled
	true, // navMeshUpdateGuardEnabled
#ifdef ZONEOPT_DEBUG
	true, // destroyListDiagEnabled
#else
	false, // destroyListDiagEnabled
#endif
	true, // destroyListDeferEnabled
	true, // unstitchGuardEnabled
	32, // cfg_stitchSourceLines
	true, // graphVisitorGuardEnabled
	true, // graphExpandGuardEnabled
	true, // graphPositionGuardEnabled
	true, // meshFaceGuardEnabled
	true, // createInstanceGuardEnabled
	true, // hullDoublePushGuardEnabled
	true, // stitchByteGuardEnabled
	true, // navMeshLifeEnabled
	false, // unstitchProbeEnabled
	true, // sectionKeyProbeEnabled
	true, // physPurecallRecordEnabled
	true, // physQueryGuardEnabled
	true, // corpsePinEnabled
	true, // nestValidationGuardEnabled
	0, // graphHeuristicGuardOn
};

FixesConfig g_fixesCfg = kFixesDefaults;
} // namespace fixes

namespace fixes_config_detail {
union FixesConfigPodCheck { fixes::FixesConfig s; };
} // namespace fixes_config_detail
using namespace fixes_config_detail;

namespace fixes {

const ConfigKey g_fixesConfigKeys[] =
{
	CFG_OBOOL("pathExtractGuard", FixesConfig, pathExtractGuardEnabled,      DOC, SHOW,
	  "Path extraction guard",
	  "A fault while reading a path result rolls it back to no new edges instead of crashing; the"
	  " character re-paths on the next tick."),
	CFG_OBOOL("sectionStamp", FixesConfig, sectionStampEnabled,          DOC, DIAG,
	  "Navmesh section insert counter",
	  "Counts every instance the navmesh streaming collection takes, on the PathGuard: log line."),
	CFG_OBOOL("navMeshUpdateGuard", FixesConfig, navMeshUpdateGuardEnabled,    DOC, SHOW,
	  "Navmesh update fault recorder",
	  "Records a fault in the navmesh thread's update pass to navmesh_guard.txt and leaves the navmesh"
	  " lock held, so the damaged data is never read."),
	CFG_OBOOL("destroyListDiag", FixesConfig, destroyListDiagEnabled,       NDOC, DIAG,
	  "Destroy list thread recorder",
	  "Records the calling thread of each insert into the game's destroy list. Changes nothing the game"
	  " does."),
	CFG_OBOOL("destroyListDefer", FixesConfig, destroyListDeferEnabled,      NDOC, SHOW,
	  "Destroy list deferral",
	  "Queues destroy list inserts made off the main thread and replays them on the main thread, so the"
	  " list has a single writer."),
	CFG_OBOOL("unstitchGuard", FixesConfig, unstitchGuardEnabled,         DOC, SHOW,
	  "Un-stitch bounds guard",
	  "Drops a stale navmesh connection record that names a node its neighbour no longer has, instead"
	  " of reading past the node map and crashing."),
	CFG_OINT("stitchSourceLines", FixesConfig, cfg_stitchSourceLines, 1.0f, 0.0f, 0, DOC, DIAG,
	  "Stitch source log sets",
	  "How many sets that lost records to the un-stitch guard are described in the log, two lines each."
	  " 0 prints none."),
	CFG_OBOOL("graphVisitorGuard", FixesConfig, graphVisitorGuardEnabled,     DOC, SHOW,
	  "Search estimate instance guard",
	  "Keeps a path search node whose section has no graph instance in the search without a distance"
	  " estimate, instead of crashing."),
	CFG_OBOOL("graphExpandGuard", FixesConfig, graphExpandGuardEnabled,      DOC, SHOW,
	  "Search expansion instance guard",
	  "Drops a path search node whose section has no graph instance, instead of crashing, and carries"
	  " on with the rest."),
	CFG_OBOOL("graphPositionGuard", FixesConfig, graphPositionGuardEnabled,    NDOC, SHOW,
	  "Cluster graph position guard",
	  "When the cluster graph is consulted, substitutes a node whose section has no graph instance"
	  " instead of crashing. Off only classifies and counts it."),
	CFG_OBOOL("meshFaceGuard", FixesConfig, meshFaceGuardEnabled,         DOC, SHOW,
	  "Navmesh face bounds guard",
	  "Leaves a navmesh face whose edge record is no longer valid out of its section's bounding box,"
	  " instead of walking outside the section and crashing."),
	CFG_OBOOL("createInstanceGuard", FixesConfig, createInstanceGuardEnabled,   DOC, SHOW,
	  "Queued navmesh piece guard",
	  "Stops the game freeing a navmesh piece that is already queued to be added. Off only counts it."),
	CFG_OBOOL("hullDoublePushGuard", FixesConfig, hullDoublePushGuardEnabled,   DOC, SHOW,
	  "Physics double delete guard",
	  "Drops a second destroy entry for one physics object before the physics thread deletes it twice."
	  " Off only counts duplicates."),
	CFG_OBOOL("stitchByteGuard", FixesConfig, stitchByteGuardEnabled,       DOC, SHOW,
	  "Interior stitch byte guard",
	  "Skips the flag byte the game writes past an interior's navmesh when its stitch finishes. Off"
	  " only records what the write hit."),
	CFG_OBOOL("navMeshLife", FixesConfig, navMeshLifeEnabled,           DOC, DIAG,
	  "Navmesh section lifecycle lines",
	  "Logs one line each time a navmesh section joins or leaves the streaming collection. Read-only."),
	CFG_OBOOL_DBG("unstitchProbe", FixesConfig, unstitchProbeEnabled,         DOC, DIAG,
	  "Un-stitch probe",
	  "Before each navmesh instance teardown, logs any connection record whose node index would land"
	  " outside its neighbour's node map. Changes nothing."),
	CFG_OBOOL_DBG("sectionKeyProbe", FixesConfig, sectionKeyProbeEnabled,       DOC, DIAG,
	  "Section key probe",
	  "Records the last few section-table lookups the world step makes from a packed key, so a crash"
	  " record says whether the index was in range. Changes nothing."),
	CFG_OBOOL("physPurecallRecord", FixesConfig, physPurecallRecordEnabled,    DOC, DIAG,
	  "PhysX pure call recorder",
	  "Captures the faulting thread and call site into purecall_dump.txt when PhysX aborts on a pure"
	  " virtual call. Forensic only."),
	CFG_OBOOL("physQueryGuard", FixesConfig, physQueryGuardEnabled,        DOC, SHOW,
	  "PhysX box query guard",
	  "Checks each result of the game's box query before it is used, and drops a collision shape that"
	  " is already destroyed."),
	CFG_OBOOL("corpsePin", FixesConfig, corpsePinEnabled,             DOC, SHOW,
	  "Carried corpse follows its carrier",
	  "Moves the squad of a carried NPC corpse to its carrier instead of leaving it pinned at the"
	  " pickup spot."),
	CFG_OBOOL("nestValidationGuard", FixesConfig, nestValidationGuardEnabled,   DOC, SHOW,
	  "Nest validation guard",
	  "Skips the game's nest validation for a cell whose navmesh is not in yet, so no nest is destroyed"
	  " against a missing mesh; the cell is checked again next loading cycle."),
	CFG_OCUSTOM("graphHeuristicGuard", FixesConfig, graphHeuristicGuardOn, ParseGraphHeuristicGuard, NDOC),
	{ NULL, CK_BOOL, 0, 0, 0.0f, 0.0f, false, NULL, NULL, false, 0.0f, 0, NULL, INT_MIN, false, false, false, NULL, NULL, NULL, NULL, 0 }
};

} // namespace fixes
