// cluster_cross_cost.h - The world-frame cost for cross-tile cluster-graph links, applied after
// the collection connects a newly registered graph instance. Installed at startup on the main
// thread when clusterCrossCost is on.
#ifndef KENSHI_ZONE_OPT_FIXES_CLUSTER_CROSS_COST_H
#define KENSHI_ZONE_OPT_FIXES_CLUSTER_CROSS_COST_H

void InstallClusterCrossCost(int* installed, int*);
const char* ClusterCrossCostToken();   // "ON" when installed, "OFF" otherwise

#endif
