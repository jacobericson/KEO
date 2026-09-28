// hook_manifest_policy.cpp — the want predicate and its inputs. Host-safe: the
// config globals come from config_values.h, which carries no game header.

#include "plugin/hook_manifest_policy.h"
#include "base/config_values.h"

bool HookWantEval(HookWant want, const HookWantInputs& in)
{
	switch (want)
	{
	case HOOK_WANT_ALWAYS:            return true;
	case HOOK_WANT_MOVEMENT_AWARE:    return in.movementAware;
	case HOOK_WANT_DESTROY_LIST:      return in.destroyListDiag || in.destroyListDefer;
	case HOOK_WANT_CACHING:           return in.caching;
	case HOOK_WANT_PATHFIND_DIAG:     return in.pathfindDiag;
	case HOOK_WANT_PATH_EXTRACT:      return in.pathfindDiag && in.pathExtractGuard;
	case HOOK_WANT_ADD_INSTANCE:      return (in.pathfindDiag && in.sectionStamp) || in.navMeshLife;
	case HOOK_WANT_GATE_PASS:         return in.gatePassDiag;
	case HOOK_WANT_ESCAPE_PAUSE:      return in.escapePauseGuard;
	case HOOK_WANT_CORPSE_PIN:        return in.corpsePin;
	case HOOK_WANT_NEST_GUARD:        return in.nestValidationGuard;
	case HOOK_WANT_UNSTITCH_GUARD:    return in.unstitchGuard;
	case HOOK_WANT_GRAPH_VISITOR:     return in.graphVisitorGuard;
	case HOOK_WANT_GRAPH_EXPAND:      return in.graphExpandGuard;
	case HOOK_WANT_MESH_FACE:         return in.meshFaceGuard;
	case HOOK_WANT_NAVMESH_LIFE:      return in.navMeshLife;
	case HOOK_WANT_UNSTITCH_PROBE:    return in.unstitchProbe;
	case HOOK_WANT_SECTION_KEY_PROBE: return in.sectionKeyProbe;
	case HOOK_WANT_UNCOUNTED:         return false;
	}
	return false;
}

HookWantInputs HookWantInputsFromConfig()
{
	HookWantInputs in;
	in.destroyListDiag     = fixes::g_fixesCfg.destroyListDiagEnabled;
	in.destroyListDefer    = fixes::g_fixesCfg.destroyListDeferEnabled;
	in.escapePauseGuard    = zone::g_zoneCfg.escapePauseGuardEnabled;
	in.corpsePin           = fixes::g_fixesCfg.corpsePinEnabled;
	in.nestValidationGuard = fixes::g_fixesCfg.nestValidationGuardEnabled;
	in.unstitchGuard       = fixes::g_fixesCfg.unstitchGuardEnabled;
	in.graphVisitorGuard   = fixes::g_fixesCfg.graphVisitorGuardEnabled;
	in.graphExpandGuard    = fixes::g_fixesCfg.graphExpandGuardEnabled;
	in.meshFaceGuard       = fixes::g_fixesCfg.meshFaceGuardEnabled;
	in.navMeshLife         = fixes::g_fixesCfg.navMeshLifeEnabled;
#ifdef ZONEOPT_DEBUG
	in.unstitchProbe       = fixes::g_fixesCfg.unstitchProbeEnabled;
	in.sectionKeyProbe     = fixes::g_fixesCfg.sectionKeyProbeEnabled;
#else
	in.unstitchProbe       = false;
	in.sectionKeyProbe     = false;
#endif
	in.movementAware       = zone::g_zoneCfg.movementAwareEnabled;
	in.caching             = navmesh::g_navmeshCfg.cachingEnabled;
	in.pathfindDiag        = pathfind::g_pathfindCfg.pathfindDiagEnabled;
	in.pathExtractGuard    = fixes::g_fixesCfg.pathExtractGuardEnabled;
	in.sectionStamp        = fixes::g_fixesCfg.sectionStampEnabled;
	in.gatePassDiag        = pathfind::g_pathfindCfg.gatePassDiagEnabled;
	return in;
}
