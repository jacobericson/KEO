// Host tests for the one-time settings import: every outcome of the pure decision in both modes,
// and the file steps against a real folder, one fresh folder per case. Each failure path is forced
// in the folder itself: a handle held with no sharing or with read sharing only, or a folder
// taking the .replaced name.

#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include "base/ini_names.h"
#include "base/legacy_ini_import.h"

#include "check.h"

static std::vector<std::string> g_lines;
static int g_case = 0;

static void RecordLine(const std::string& line)
{
	g_lines.push_back(line);
}

static const char* const FIXTURE_NAMES[] = {
	"old.ini", "old.ini.imported", "new.ini", "new.ini.replaced", "new.ini.tmp", "stale.txt"
};

static void Wipe(const std::string& dir)
{
	for (size_t i = 0; i < sizeof(FIXTURE_NAMES) / sizeof(FIXTURE_NAMES[0]); ++i)
	{
		std::string p = dir + FIXTURE_NAMES[i];
		if (!DeleteFileA(p.c_str()))
			RemoveDirectoryA(p.c_str());
	}
	RemoveDirectoryA(dir.c_str());
}

// A fresh, empty folder for one case, with the trailing backslash a DLL's folder carries.
static std::string NewFolder()
{
	char tmp[MAX_PATH];
	GetTempPathA(sizeof(tmp), tmp);
	char dir[MAX_PATH];
	_snprintf_s(dir, sizeof(dir), _TRUNCATE, "%skeo_legacy_ini_units_%lu_%d\\",
	            tmp, (unsigned long)GetCurrentProcessId(), ++g_case);
	Wipe(dir);
	CreateDirectoryA(dir, NULL);
	g_lines.clear();
	return dir;
}

static void Put(const std::string& path, const std::string& bytes)
{
	FILE* f = NULL;
	if (fopen_s(&f, path.c_str(), "wb") == 0 && f)
	{
		fwrite(bytes.data(), 1, bytes.size(), f);
		fclose(f);
	}
}

static bool Exists(const std::string& path)
{
	return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool Holds(const std::string& path, const std::string& bytes)
{
	FILE* f = NULL;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
		return false;
	std::string got;
	char buf[512];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		got.append(buf, n);
	fclose(f);
	return got == bytes;
}

static HANDLE Hold(const std::string& path, DWORD share)
{
	return CreateFileA(path.c_str(), GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static bool OneLine(const std::string& want)
{
	return g_lines.size() == 1 && g_lines[0] == want;
}

// The one recorded line, when it is head + "<n>" + tail for some number n.
static bool OneLineWithError(const std::string& head, const std::string& tail)
{
	if (g_lines.size() != 1)
		return false;
	const std::string& l = g_lines[0];
	if (l.size() <= head.size() + tail.size() || l.compare(0, head.size(), head) != 0
	    || l.compare(l.size() - tail.size(), tail.size(), tail) != 0)
		return false;
	std::string n = l.substr(head.size(), l.size() - head.size() - tail.size());
	return n.find_first_not_of("0123456789") == std::string::npos;
}

static LegacyIniInputs Inputs(bool oldExists, const char* oldText, bool markerExists,
                              bool newExists, const char* newText, bool refuse)
{
	LegacyIniInputs in;
	in.oldExists                = oldExists;
	in.oldText                  = oldText;
	in.markerExists             = markerExists;
	in.newExists                = newExists;
	in.newText                  = newText;
	in.refuseWhenNewHasLiveLine = refuse;
	return in;
}

static LegacyIniOutcome Run(const std::string& dir, bool refuse)
{
	return LegacyIniImport(dir, "old.ini", "new.ini", NULL, refuse, &RecordLine);
}

static void DecideRows()
{
	Check(LegacyIniDecide(Inputs(false, "", false, false, "", true)) == LEGACY_INI_NONE
	      && LegacyIniDecide(Inputs(false, "a=1\n", true, true, "b=2\n", false)) == LEGACY_INI_NONE,
	      "decide: no old file is NONE");

	Check(LegacyIniDecide(Inputs(true, "a=1\n", true, false, "", true)) == LEGACY_INI_LEFT_ALONE_DONE
	      && LegacyIniDecide(Inputs(true, "a=1\n", true, false, "", false)) == LEGACY_INI_LEFT_ALONE_DONE
	      && LegacyIniDecide(Inputs(true, "a=1\n", true, true, "b=2\n", true)) == LEGACY_INI_LEFT_ALONE_DONE
	      && LegacyIniDecide(Inputs(true, "a=1\n", true, true, "b=2\n", false)) == LEGACY_INI_LEFT_ALONE_DONE,
	      "decide: a marker is LEFT_ALONE_DONE in both modes");

	const char* noLive = "# settings\r\n\r\n[Render]\r\n; a=1\r\nkey=\r\n=value\r\n";
	Check(LegacyIniDecide(Inputs(true, noLive, false, false, "", true)) == LEGACY_INI_RENAME_ONLY
	      && LegacyIniDecide(Inputs(true, noLive, false, false, "", false)) == LEGACY_INI_RENAME_ONLY
	      && LegacyIniDecide(Inputs(true, noLive, false, true, "a=1\n", true)) == LEGACY_INI_RENAME_ONLY
	      && LegacyIniDecide(Inputs(true, "", false, true, "a=1\n", false)) == LEGACY_INI_RENAME_ONLY,
	      "decide: an old file with no live line is RENAME_ONLY");

	const char* oldSame = "a=1\r\n# old comment\r\n\r\nb = 2\r\n";
	const char* newSame = "; new header\n  a =1\n\n[Section]\nb=2";
	Check(LegacyIniDecide(Inputs(true, oldSame, false, true, newSame, true)) == LEGACY_INI_RENAME_ONLY
	      && LegacyIniDecide(Inputs(true, oldSame, false, true, newSame, false)) == LEGACY_INI_RENAME_ONLY
	      && LegacyIniDecide(Inputs(true, "b=2\na=1\n", false, true, "a=1\nb=2\n", false)) == LEGACY_INI_IMPORT
	      && LegacyIniDecide(Inputs(true, "a=1\n", false, true, "a=2\n", false)) == LEGACY_INI_IMPORT,
	      "decide: equal live lines are RENAME_ONLY (comments, blanks and spacing ignored)");

	Check(LegacyIniDecide(Inputs(true, "a=1\n", false, true, "a=2\n", true)) == LEGACY_INI_LEFT_ALONE_NEW_SET
	      && LegacyIniDecide(Inputs(true, "a=1\n", false, true, "# a=2\n\n", true)) == LEGACY_INI_IMPORT,
	      "decide: optimizer mode refuses a new file with a live line");

	Check(LegacyIniDecide(Inputs(true, "a=1\n", false, true, "a=2\nb=3\n", false)) == LEGACY_INI_IMPORT,
	      "decide: profiler mode imports over a new file with live lines");

	Check(LegacyIniDecide(Inputs(true, "a=1\n", false, false, "", true)) == LEGACY_INI_IMPORT
	      && LegacyIniDecide(Inputs(true, "a=1\n", false, false, "", false)) == LEGACY_INI_IMPORT,
	      "decide: a live old file and no new file is IMPORT");
}

static void ImportRows()
{
	const std::string oldBytes = "# kept as written\r\na=1\r\nb = 2\r\n";
	const std::string newBytes = "x=9\r\n# hand edit\r\n";

	std::string d = NewFolder();
	Put(d + "old.ini", oldBytes);
	LegacyIniOutcome r = Run(d, true);
	Check(r == LEGACY_INI_IMPORT && Holds(d + "new.ini", oldBytes) && Holds(d + "old.ini.imported", oldBytes)
	      && !Exists(d + "old.ini") && !Exists(d + "new.ini.tmp")
	      && OneLine("LegacyIni: old.ini imported into new.ini (previous file kept as old.ini.imported)"),
	      "import: the new file holds the old bytes and the marker exists");
	Check(r == LEGACY_INI_IMPORT && !Exists(d + "new.ini.replaced"), "import: no .replaced without a new file");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	r = Run(d, false);
	bool profilerMode = r == LEGACY_INI_IMPORT && Holds(d + "new.ini.replaced", newBytes)
	                    && Holds(d + "new.ini", oldBytes) && Holds(d + "old.ini.imported", oldBytes)
	                    && OneLine("LegacyIni: old.ini imported into new.ini (previous file kept as "
	                               "old.ini.imported; previous new.ini kept as new.ini.replaced)");
	Wipe(d);
	d = NewFolder();
	const std::string commentsOnly = "# nothing set yet\n";
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", commentsOnly);
	r = Run(d, true);
	bool optimizerMode = r == LEGACY_INI_IMPORT && Holds(d + "new.ini.replaced", commentsOnly)
	                     && Holds(d + "new.ini", oldBytes);
	Check(profilerMode && optimizerMode, "import: a replaced new file is kept as .replaced, byte for byte");
	Wipe(d);

	d = NewFolder();
	const std::string templ = "# every key at its default\r\n";
	Put(d + "old.ini", templ);
	Put(d + "new.ini", newBytes);
	r = Run(d, true);
	Check(r == LEGACY_INI_RENAME_ONLY && Holds(d + "new.ini", newBytes) && Holds(d + "old.ini.imported", templ)
	      && !Exists(d + "old.ini") && !Exists(d + "new.ini.replaced")
	      && OneLine("LegacyIni: old.ini renamed to old.ini.imported (no settings to carry)"),
	      "rename only: the new file is untouched");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	r = Run(d, true);
	bool newSet = r == LEGACY_INI_LEFT_ALONE_NEW_SET && Holds(d + "old.ini", oldBytes)
	              && Holds(d + "new.ini", newBytes) && !Exists(d + "old.ini.imported")
	              && OneLine("LegacyIni: old.ini left alone (new.ini has settings)");
	Put(d + "old.ini.imported", "a=0\n");
	g_lines.clear();
	r = Run(d, false);
	bool done = r == LEGACY_INI_LEFT_ALONE_DONE && Holds(d + "old.ini", oldBytes)
	            && Holds(d + "new.ini", newBytes) && Holds(d + "old.ini.imported", "a=0\n")
	            && OneLine("LegacyIni: old.ini left alone (already imported)");
	Check(newSet && done, "left alone: both files untouched");
	Wipe(d);
}

static void MarkerAndLogRows()
{
	std::string d = NewFolder();
	Put(d + "old.ini", "a=1\n");
	LegacyIniOutcome first = Run(d, false);
	const std::string edited = "a=1\nb=5\n";
	Put(d + "new.ini", edited);
	Put(d + "old.ini", "a=7\nc=3\n");
	g_lines.clear();
	LegacyIniOutcome second = Run(d, false);
	Check(first == LEGACY_INI_IMPORT && second == LEGACY_INI_LEFT_ALONE_DONE && Holds(d + "new.ini", edited)
	      && Holds(d + "old.ini.imported", "a=1\n") && Holds(d + "old.ini", "a=7\nc=3\n")
	      && !Exists(d + "new.ini.replaced"),
	      "marker row (profiler mode): after an import, an edit and a different old file, the second run is LEFT_ALONE_DONE and the new file is unchanged");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", "a=1\n");
	Put(d + "new.ini", "a=2\n");
	Run(d, false);
	Check(g_lines.size() == 1 && g_lines[0].find("LegacyIni: ") == 0
	      && g_lines[0].find("old.ini") != std::string::npos && g_lines[0].find("new.ini") != std::string::npos,
	      "log: profiler mode writes exactly one LegacyIni: line naming its files");
	Wipe(d);

	d = NewFolder();
	Put(d + "new.ini", "a=2\n");
	LegacyIniOutcome r = Run(d, true);
	LegacyIniOutcome r2 = Run(d, false);
	Check(r == LEGACY_INI_NONE && r2 == LEGACY_INI_NONE && g_lines.empty() && Holds(d + "new.ini", "a=2\n"),
	      "log: no old file, no line");
	Wipe(d);

	d = NewFolder();
	Put(d + "stale.txt", "stale\n");
	r = LegacyIniImport(d, "old.ini", "new.ini", "stale.txt", true, &RecordLine);
	bool deleted = r == LEGACY_INI_NONE && !Exists(d + "stale.txt");
	r = LegacyIniImport(d, "old.ini", "new.ini", "stale.txt", true, &RecordLine);
	Check(deleted && r == LEGACY_INI_NONE && g_lines.empty(),
	      "stale: the stale file is deleted, and an absent one is no error");
	Wipe(d);
}

static void FailureRows()
{
	const std::string oldBytes = "a=1\n";
	const std::string newBytes = "a=2\n";

	std::string d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	HANDLE h = Hold(d + "old.ini", 0);
	LegacyIniOutcome r = Run(d, false);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	Check(h != INVALID_HANDLE_VALUE && r == LEGACY_INI_FAILED
	      && OneLineWithError("LegacyIni: old.ini import failed (read error ", ")")
	      && Holds(d + "old.ini", oldBytes) && Holds(d + "new.ini", newBytes)
	      && !Exists(d + "old.ini.imported") && !Exists(d + "new.ini.tmp"),
	      "failure: an unreadable old file is FAILED (read error), both files untouched");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	h = Hold(d + "old.ini", FILE_SHARE_READ);
	r = Run(d, false);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	Check(h != INVALID_HANDLE_VALUE && r == LEGACY_INI_FAILED
	      && OneLineWithError("LegacyIni: old.ini import failed (rename error ", ")")
	      && Holds(d + "old.ini", oldBytes) && Holds(d + "new.ini", newBytes)
	      && !Exists(d + "old.ini.imported") && !Exists(d + "new.ini.replaced") && !Exists(d + "new.ini.tmp"),
	      "failure: a rename refused is FAILED (rename error), no write and no marker");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	CreateDirectoryA((d + "new.ini.replaced").c_str(), NULL);
	r = Run(d, false);
	Check(r == LEGACY_INI_FAILED
	      && OneLineWithError("LegacyIni: old.ini import failed (replaced error ",
	                          "); settings kept in old.ini.imported, new.ini untouched")
	      && Holds(d + "new.ini", newBytes) && Holds(d + "old.ini.imported", oldBytes)
	      && !Exists(d + "old.ini") && !Exists(d + "new.ini.tmp"),
	      "failure: a refused .replaced copy is FAILED (replaced error), the new file untouched");
	Wipe(d);

	d = NewFolder();
	Put(d + "old.ini", oldBytes);
	Put(d + "new.ini", newBytes);
	h = Hold(d + "new.ini", FILE_SHARE_READ);
	r = Run(d, false);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	Check(h != INVALID_HANDLE_VALUE && r == LEGACY_INI_FAILED
	      && OneLineWithError("LegacyIni: old.ini import failed (write error ", "); settings kept in old.ini.imported")
	      && Holds(d + "new.ini", newBytes) && Holds(d + "new.ini.replaced", newBytes)
	      && Holds(d + "old.ini.imported", oldBytes) && !Exists(d + "new.ini.tmp"),
	      "failure: a refused move over the new file is FAILED (write error), the .tmp deleted");
	Wipe(d);
}

static void NameRows()
{
	const std::string optNew(OPTIMIZER_INI_NAME);
	const std::string profNew(PROFILER_INI_NAME);
	const std::string optOld(LEGACY_OPTIMIZER_INI_NAME);
	const std::string profOld(LEGACY_PROFILER_INI_NAME);
	const std::string retireOld(LEGACY_OPTIMIZER_RETIRE_NAME);
	Check(optNew == "KEO.ini" && profNew == "KEOProfiler.ini"
	      && !optOld.empty() && !profOld.empty() && !retireOld.empty()
	      && optOld != optNew && optOld != profNew && profOld != optNew && profOld != profNew
	      && optOld != profOld && retireOld != OPTIMIZER_RETIRE_NAME,
	      "names: the current names are KEO.ini and KEOProfiler.ini, the previous ones differ from them");
}

int main()
{
	DecideRows();
	ImportRows();
	MarkerAndLogRows();
	FailureRows();
	NameRows();
	return CheckExit("legacy_ini_import_units");
}
