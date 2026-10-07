// nm_cache_type_policy.cpp - which navmesh jobs the L1/L2 cache may key.
#include "navmesh/cache/nm_cache_type_policy.h"

namespace navmesh {

bool NmJobCacheable(int jobType)
{
	return jobType == 0;
}

} // namespace navmesh
