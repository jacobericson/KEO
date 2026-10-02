// cluster_cross_cost.h - The world-frame cost for cross-tile cluster-graph links, applied after
// the collection connects a newly registered graph instance. Installed at startup on the main
// thread when clusterCrossCost or plannerMode is set.
#ifndef KEO_FIXES_CLUSTER_CROSS_COST_H
#define KEO_FIXES_CLUSTER_CROSS_COST_H

void InstallClusterCrossCost(int* installed, int*);
const char* ClusterCrossCostToken();   // "ON" when installed with clusterCrossCost on, "OFF" otherwise

#endif
