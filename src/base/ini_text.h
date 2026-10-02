#pragma once
// A build line passing the old DEV define name stops here.
#ifdef ZONEOPT_DEBUG
#error stale DEV define: the build lines pass /DKEO_DEBUG
#endif
#include <string>
#include <vector>

// KenshiZoneOpt.ini line rules, shared by the loader and the writer so both
// recognise exactly the same lines. No Windows calls.

std::string IniTrim(const std::string& s);

// True for a key=value line: not blank, not a # or ; comment, not a
// [section], with a non-empty key and value (both trimmed). The value is
// everything after the first '=': there are no inline comments.
bool SplitIniLine(const std::string& line, std::string* key, std::string* val);

// INI value parsers; false leaves *out untouched.
bool ParseBool(const std::string& val, bool* out);
bool ParseInt(const std::string& val, int* out);
bool ParseFloat(const std::string& val, float* out);

// A bool that also accepts one named third position, case-insensitively.
// *outIsThird says which of the two answers came back, so a caller never has
// to compare the text itself. False leaves both outputs untouched.
bool ParseBoolOr(const std::string& val, const char* thirdWord,
                 bool* outBool, bool* outIsThird);

enum IniValueKind { INI_BOOL, INI_INT, INI_FLOAT, INI_TEXT };

// One key the writer puts in the file.
struct IniEntry
{
	std::string  key;
	std::string  value;    // the text written
	IniValueKind kind;     // how a value already in the file is compared with it
	bool         append;   // add a line when the file has no valid one for key
};

// Whether existing (a value in the file) already means value: both parsed
// by kind's parser (floats by their bits), text compared exactly. An
// existing value that does not parse never matches.
bool IniValueEquals(IniValueKind kind, const std::string& existing, const std::string& value);

// Writes entries into INI text by the loader's line rules. Every valid line
// naming an entry's key whose value differs gets the new value in place (the
// key, its spacing, the line's ending and trailing blanks kept); every other
// line stays byte for byte. Entries marked append that have no valid line go
// after the last non-blank line of section appendSection (e.g. "[Render]",
// matched case-insensitively) when there is one, else at the end of the
// text. With appendSection NULL they go after the last non-blank line before
// the first section header, so they stay outside every section (at the end
// when there is no header). Added lines take the text's first line ending
// (CRLF when it has none). A last line without an ending gets one when lines
// are appended after it.
std::string RewriteIniKeys(const std::string& text, const std::vector<IniEntry>& entries,
                           const char* appendSection);

// One key applied while loading a single INI file, tracking the first and
// most recent line that set it (a key never applied once still has
// firstLine == lastLine, so seeing both equal means "not a duplicate").
struct IniDupSeen
{
	std::string key;
	int         firstLine;
	int         lastLine;
};

// Records that `key` took effect at `line`. The loader calls this once per
// accepted key=value line, in file order, so lastLine always ends up the
// line whose value actually stuck (last-wins). `seen` is the caller's own
// per-load list; keep it local, never static (see the config.cpp global
// constructor rule).
void IniNoteAppliedKey(std::vector<IniDupSeen>& seen, const std::string& key, int line);

// The duplicate-key notice for one entry that repeated. Only meaningful when
// seen.firstLine != seen.lastLine.
std::string IniDupMessage(const IniDupSeen& seen);
