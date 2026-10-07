// backpack_sidecar_policy.h - The backpack-first sidecar's text, pure: one version line, then
// one line per character whose choice differs from the INI default. Parsing is tolerant: a bad
// line is skipped and counted; a missing or unknown version line refuses the whole file.
#ifndef KEO_INVENTORY_BACKPACK_SIDECAR_POLICY_H
#define KEO_INVENTORY_BACKPACK_SIDECAR_POLICY_H

#include <string>
#include "game/hand_key.h"

namespace keo_inventory {

const char* const SIDECAR_FILE_NAME = "KEO_backpacks.txt";
const char* const SIDECAR_HEADER    = "KEO_backpacks 1";

struct SidecarEntry { game::HandKey key; int on; };

// The version line, then "<type> <container> <containerSerial> <index> <serial> <0|1>" for each
// entry whose on differs from defaultOn, in the given order, each ended by "\n". An empty list
// (or every entry equal to the default) is the version line alone.
std::string SidecarFormat(const SidecarEntry* entries, int n, bool defaultOn);

struct SidecarParseResult
{
	bool refused;      // no version line, or another version: nothing accepted
	int  accepted;     // entries written to out
	int  badLines;     // skipped: wrong field count, a non-number, on not 0 or 1, a null key
	int  duplicates;   // a key seen again: its last line wins
	int  overflow;     // well-formed lines past max
};
// CR before LF is accepted; blank lines and lines starting with '#' are skipped; a last line
// without a newline is read.
SidecarParseResult SidecarParse(const char* text, int len, SidecarEntry* out, int max);

} // namespace keo_inventory

#endif
