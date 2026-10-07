// nm_cache_type_policy.h - which navmesh jobs the L1/L2 cache may key. Pure: no game or Windows
// header; any thread.
#ifndef KEO_NM_CACHE_TYPE_POLICY_H
#define KEO_NM_CACHE_TYPE_POLICY_H

namespace navmesh {

// Whether a navmesh job of this type (its task's flags & 7) may be looked up in or stored to the
// L1/L2 cache, or join the duplicate-key table. Only a whole-zone job (type 0) may: a local patch
// (type 1) depends on the state of the buildings it covers and on the zone's mesh before it, and no
// key carries either. Types 2-4 never reach the cache.
bool NmJobCacheable(int jobType);

} // namespace navmesh

#endif
