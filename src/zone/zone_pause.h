#ifndef KENSHI_ZONE_OPT_ZONE_PAUSE_H
#define KENSHI_ZONE_OPT_ZONE_PAUSE_H

// Hooks ZoneManager::processLoading so a loader unpause that lands while the
// escape menu is open does not resume the game behind the menu. Main thread,
// startPlugin. Adds 1 to *installed when it went in.
void InstallZonePauseGuard(int* installed, int*);

// True while GameWorld::paused is set -- the same field this file's own
// escape-menu guard reads (togglePause(gw,false) keeps it only when
// frameSpeedMult == 0). Always available, whether or not the guard above is
// installed: any stall clock, arm window or hysteresis timer that must not
// advance while the game is paused reads this instead of taking its own copy.
bool ZonePauseIsPaused();

#endif
