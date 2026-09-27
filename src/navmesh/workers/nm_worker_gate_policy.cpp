#include "navmesh/workers/nm_worker_gate_policy.h"
#include <stddef.h>

NmPoolDecision NmPoolDecide(bool lazyInstallDone, bool stopHookInstalled, bool buildHookInstalled)
{
	if (!lazyInstallDone)
		return NMPOOL_WAIT;
	if (!stopHookInstalled)
		return NMPOOL_REFUSE_STOPHOOK;   // first: it guards the Havok heap's release
	if (!buildHookInstalled)
		return NMPOOL_REFUSE_BUILDHOOK;
	return NMPOOL_CREATE;
}

const char* NmPoolRefusalToken(NmPoolDecision decision)
{
	switch (decision)
	{
	case NMPOOL_REFUSE_STOPHOOK:  return "refused(stopHook)";
	case NMPOOL_REFUSE_BUILDHOOK: return "refused(buildHook)";
	default:                      return NULL;
	}
}
