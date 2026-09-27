// plugin_entry_internal.h - Private declarations for the plugin entry.
// Main thread only; logging uses the core log lock.
#ifndef KENSHI_ZONE_OPT_PLUGIN_ENTRY_INTERNAL_H
#define KENSHI_ZONE_OPT_PLUGIN_ENTRY_INTERNAL_H

#include <string>

namespace plugin_entry_detail
{
void LogInitBanner(int installed, int totalHooks, const std::string& gateTok,
                  int renderInstalled, int renderWanted, const std::string& benchTok);
void LogKlibBinding(const char* message);
}

#endif
