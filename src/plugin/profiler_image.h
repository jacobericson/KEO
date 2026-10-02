#ifndef KEO_PLUGIN_PROFILER_IMAGE_H
#define KEO_PLUGIN_PROFILER_IMAGE_H

// The optimizer's lookup of the profiler's image, as the camera tick drives it.

// Main thread, every frame: drives the profiler lookup's 60 s window and, when it closes,
// writes the one ProfilerImage: line. Logging takes the core log lock.
void ProfilerImageTickMain(double now);

#endif
