// preload.cpp - The preload working tables: queues, tracked zones and session counters. State only; main thread.
#include "zone/preload/preload.h"


PreloadedZone preloadedZones[MAX_PRELOADED];
int numPreloaded     = 0;
int pendingCount     = 0;
int predictedCenterX = -1;
int predictedCenterY = -1;

QueuedZone cameraQueue[CAMERA_RESERVED];
int cameraQueueCount = 0;
int cameraQueueNext  = 0;

QueuedZone charQueue[MAX_PRELOADED];
int charQueueCount = 0;
int charQueueNext  = 0;

double lastCharScanTime = 0.0;
int charZonesQueued     = 0;
double lastCamLogTime   = 0.0;
int preloadSkipLoaded   = 0;
int lastCameraGX         = -1;
int lastCameraGY         = -1;
int regSkipCount         = 0;

