#include "render/render_config.h"
#include "render/render_keys.h"
#include "render/render_levers.h"
#include "base/core.h"
#include "base/ini_names.h"
#include <cerrno>
#include <cstdio>
#include <cstring>

// Local wall-clock time to the millisecond: the join key between a change
// and the frame audit's per-second CSV.
static std::string WallClockStamp()
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char buf[32];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02u:%02u:%02u.%03u",
	            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

// Every field is written from the main thread, one aligned store at a time;
// the detours on other threads read their key per call, so they see either
// the old or the new value of each key and need no lock.
bool ApplyRenderConfig(const RenderConfig& next)
{
	if (!IsMainThread())
	{
		LogMsg("Render config: apply refused off the main thread");
		return false;
	}
	RenderConfig want = next;
	std::vector<std::string> notes;
	ClampRenderValues(&want, g_renderCfg, &notes);
	for (size_t i = 0; i < notes.size(); ++i)
		LogMsg("Render config: " + notes[i]);

	std::vector<RenderChange> changes;
	DiffRenderConfig(g_renderCfg, want, &changes);
	if (changes.empty())
		return false;

	std::string stamp = WallClockStamp();
	bool applied = false;
	for (size_t i = 0; i < changes.size(); ++i)
	{
		const RenderChange& c = changes[i];
		std::string line = std::string("Render config: ") + c.key->name + " " + c.before + " -> " + c.after;
		if (!c.key->live)
		{
			LogMsg(line + " not applied, read at startup only");
			continue;
		}
		LogMsg(line + " at " + stamp);
		CopyRenderValue(&g_renderCfg, want, *c.key);
		applied = true;
		// A number keys a lever too (a budget); 0 is its off.
		bool on = c.key->kind == RK_BOOL ? c.after == "true" : (c.key->kind == RK_FLOAT && c.after != "0");
		if (on)
		{
			std::string why = RenderLeverInertReason(c.key->name);
			if (!why.empty())
				LogMsg(std::string("Render config: ") + c.key->name + " on, but " + why);
		}
	}
	return applied;
}

// The whole file; false when it exists but cannot be read. A missing file
// reads as empty text, so the first save creates it.
static bool ReadWholeFile(const std::string& path, std::string* text)
{
	text->clear();
	FILE* f = NULL;
	errno_t err = fopen_s(&f, path.c_str(), "rb");
	if (err != 0 || !f)
		return err == ENOENT;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text->append(buf, n);
	bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
}

// Written to a temporary file first and moved over the INI, so the INI is
// always either the old text or the new one. Returns 0, or the Win32 error
// of the step that failed (ERROR_WRITE_FAULT for the CRT write).
static DWORD WriteReplacing(const std::string& path, const std::string& text)
{
	std::string tmp = path + ".tmp";
	FILE* f = NULL;
	if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f)
		return ERROR_OPEN_FAILED;
	bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
	written = (fclose(f) == 0) && written;
	DWORD err = ERROR_WRITE_FAULT;
	if (written)
		err = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
		      ? 0 : GetLastError();
	if (err)
		DeleteFileA(tmp.c_str());
	return err;
}

static std::string IniPath()
{
	return GetDLLDirectory() + OPTIMIZER_INI_NAME;
}

// Reads the INI at path; false (and a log line) when it exists but cannot be
// read.
static bool ReadIni(const std::string& path, std::string* text, const char* logPrefix)
{
	if (!ReadWholeFile(path, text))
	{
		LogMsg(std::string(logPrefix) + ": " + OPTIMIZER_INI_NAME + " could not be read, not saved");
		return false;
	}
	return true;
}

// Replaces the INI at path with out when it differs from text (the file's
// own contents just read), atomically. True with no write when nothing
// changed; false (and a log line) when the replace failed.
static bool WriteIniIfChanged(const std::string& path, const std::string& text, const std::string& out,
                              const char* logPrefix)
{
	if (out == text)
		return true;
	DWORD err = WriteReplacing(path, out);
	if (err)
	{
		std::ostringstream ss;
		ss << logPrefix << ": " << OPTIMIZER_INI_NAME << " could not be replaced (error " << err << "), not saved";
		LogMsg(ss.str());
		return false;
	}
	LogMsg(std::string(logPrefix) + ": saved to " + OPTIMIZER_INI_NAME);
	return true;
}

static const char* const RENDER_LOG_PREFIX = "Render config";

bool SaveRenderConfig(const RenderConfig& desired, const std::vector<IniEntry>& extra)
{
	std::string path = IniPath();
	std::string text;
	if (!ReadIni(path, &text, RENDER_LOG_PREFIX))
		return false;
	RenderConfig want = desired;
	std::vector<std::string> notes;
	ClampRenderValues(&want, g_renderCfg, &notes);
	std::string out = RewriteRenderIni(RewriteIniKeys(text, extra, NULL), want, RenderConfigDefaults());
	return WriteIniIfChanged(path, text, out, RENDER_LOG_PREFIX);
}

// The text's first line ending; CRLF when it has none, matching
// RewriteIniKeys' own rule for a line it adds.
static std::string FirstIniEol(const std::string& text)
{
	size_t nl = text.find('\n');
	if (nl == std::string::npos)
		return "\r\n";
	return (nl > 0 && text[nl - 1] == '\r') ? "\r\n" : "\n";
}

// True when text has a [section] line, trimmed, equal to section
// case-insensitively -- the same match RewriteIniKeys uses for appendSection.
static bool HasIniSection(const std::string& text, const char* section)
{
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? text.size() : nl + 1;
		std::string trimmed = IniTrim(line);
		if (!trimmed.empty() && trimmed[0] == '[' && _stricmp(trimmed.c_str(), section) == 0)
			return true;
	}
	return false;
}

bool SaveIniEntries(const std::vector<IniEntry>& entries, const char* section, const char* logPrefix)
{
	std::string path = IniPath();
	std::string text;
	if (!ReadIni(path, &text, logPrefix))
		return false;

	std::string work = text;
	if (section && !HasIniSection(work, section))
	{
		std::string eol = FirstIniEol(work);
		if (!work.empty() && work[work.size() - 1] != '\n')
			work += eol;
		work += section;
		work += eol;
	}
	std::string out = RewriteIniKeys(work, entries, section);
	return WriteIniIfChanged(path, text, out, logPrefix);
}
