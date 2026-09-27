#ifndef ZONEOPT_SECTION_KEY_PROBE_H
#define ZONEOPT_SECTION_KEY_PROBE_H

// Read-only capture of the section-table lookups the navmesh step makes from a
// packed key. DEV builds only.
void InstallSectionKeyProbe(int* installed, int*);

#endif
