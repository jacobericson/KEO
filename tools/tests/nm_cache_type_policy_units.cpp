// The cache's job-type rule: only a whole-zone job is keyed.
#include <cstdio>
#include "navmesh/cache/nm_cache_type_policy.h"

#include "check.h"

int main()
{
	Check(navmesh::NmJobCacheable(0), "type 0 cacheable");
	Check(!navmesh::NmJobCacheable(1), "type 1 never cacheable");
	bool none = true;
	for (int t = 2; t <= 4; ++t)
		none = none && !navmesh::NmJobCacheable(t);
	Check(none, "types 2-4 not cacheable");
	Check(!navmesh::NmJobCacheable(-1) && !navmesh::NmJobCacheable(7), "an out-of-range type is not cacheable");
	return CheckExit("nm_cache_type_policy_units");
}
