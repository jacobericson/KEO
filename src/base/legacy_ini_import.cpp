#include "base/legacy_ini_import.h"
#include "base/ini_text.h"
#include <windows.h>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

// The key and value of every line SplitIniLine accepts, in file order. Lines are split on '\n'
// with a trailing '\r' dropped.
static std::vector<std::pair<std::string, std::string> > LiveLinesOf(const std::string& text)
{
	std::vector<std::pair<std::string, std::string> > out;
	size_t pos = 0;
	for (;;)
	{
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		std::string key, val;
		if (SplitIniLine(line, &key, &val))
			out.push_back(std::make_pair(key, val));
		if (nl == std::string::npos)
			break;
		pos = nl + 1;
	}
	return out;
}

LegacyIniOutcome LegacyIniDecide(const LegacyIniInputs& in)
{
	if (!in.oldExists)
		return LEGACY_INI_NONE;
	if (in.markerExists)
		return LEGACY_INI_LEFT_ALONE_DONE;
	std::vector<std::pair<std::string, std::string> > oldLive = LiveLinesOf(in.oldText);
	std::vector<std::pair<std::string, std::string> > newLive;
	if (in.newExists)
		newLive = LiveLinesOf(in.newText);
	if (oldLive.empty() || oldLive == newLive)
		return LEGACY_INI_RENAME_ONLY;
	if (in.refuseWhenNewHasLiveLine && !newLive.empty())
		return LEGACY_INI_LEFT_ALONE_NEW_SET;
	return LEGACY_INI_IMPORT;
}

static bool PathExists(const std::string& path)
{
	return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The whole file as bytes. Returns 0, or the Win32 error of the step that failed.
static DWORD ReadWhole(const std::string& path, std::string* out)
{
	out->clear();
	HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return GetLastError();
	DWORD err = 0;
	char buf[4096];
	for (;;)
	{
		DWORD got = 0;
		if (!ReadFile(h, buf, sizeof(buf), &got, NULL))
		{
			err = GetLastError();
			break;
		}
		if (got == 0)
			break;
		out->append(buf, got);
	}
	CloseHandle(h);
	return err;
}

// Written to path.tmp first and moved over path. Returns 0, or the Win32 error of the step that
// failed (ERROR_WRITE_FAULT for the CRT write); the .tmp never outlives a failure.
static DWORD WriteReplacing(const std::string& path, const std::string& bytes)
{
	std::string tmp = path + ".tmp";
	FILE* f = NULL;
	if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f)
		return ERROR_OPEN_FAILED;
	bool written = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	written = (fclose(f) == 0) && written;
	DWORD err = ERROR_WRITE_FAULT;
	if (written)
		err = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
		      ? 0 : GetLastError();
	if (err)
		DeleteFileA(tmp.c_str());
	return err;
}

static void LogFailure(LegacyIniLogFn log, const char* oldName, const char* step, DWORD err,
                       const std::string& tail)
{
	std::ostringstream ss;
	ss << "LegacyIni: " << oldName << " import failed (" << step << " error "
	   << (unsigned long)err << ")" << tail;
	log(ss.str());
}

LegacyIniOutcome LegacyIniImport(const std::string& dir, const char* oldName, const char* newName,
                                 const char* staleName, bool refuseWhenNewHasLiveLine, LegacyIniLogFn log)
{
	if (dir.empty())
	{
		log("LegacyIni: " + std::string(oldName) + " import skipped (DLL folder unknown)");
		return LEGACY_INI_NONE;
	}

	if (staleName && *staleName)
		DeleteFileA((dir + staleName).c_str());

	const std::string oldPath    = dir + oldName;
	const std::string markerPath = oldPath + ".imported";
	const std::string newPath    = dir + newName;

	LegacyIniInputs in;
	in.oldExists                = PathExists(oldPath);
	in.markerExists             = PathExists(markerPath);
	in.newExists                = PathExists(newPath);
	in.refuseWhenNewHasLiveLine = refuseWhenNewHasLiveLine;
	if (!in.oldExists)
		return LEGACY_INI_NONE;

	DWORD err = ReadWhole(oldPath, &in.oldText);
	if (!err && in.newExists)
		err = ReadWhole(newPath, &in.newText);
	if (err)
	{
		LogFailure(log, oldName, "read", err, std::string());
		return LEGACY_INI_FAILED;
	}

	const LegacyIniOutcome outcome = LegacyIniDecide(in);
	const std::string oldBare(oldName);
	const std::string newBare(newName);
	switch (outcome)
	{
	case LEGACY_INI_LEFT_ALONE_DONE:
		log("LegacyIni: " + oldBare + " left alone (already imported)");
		return outcome;
	case LEGACY_INI_LEFT_ALONE_NEW_SET:
		log("LegacyIni: " + oldBare + " left alone (" + newBare + " has settings)");
		return outcome;
	case LEGACY_INI_RENAME_ONLY:
		if (!MoveFileExA(oldPath.c_str(), markerPath.c_str(), 0))
		{
			LogFailure(log, oldName, "rename", GetLastError(), std::string());
			return LEGACY_INI_FAILED;
		}
		log("LegacyIni: " + oldBare + " renamed to " + oldBare + ".imported (no settings to carry)");
		return outcome;
	case LEGACY_INI_IMPORT:
		break;
	default:
		return outcome;
	}

	// The marker is the commit point: once the old file carries its name, no later run imports
	// again, and nothing is written unless the rename succeeded.
	if (!MoveFileExA(oldPath.c_str(), markerPath.c_str(), 0))
	{
		LogFailure(log, oldName, "rename", GetLastError(), std::string());
		return LEGACY_INI_FAILED;
	}
	if (in.newExists && !CopyFileA(newPath.c_str(), (newPath + ".replaced").c_str(), FALSE))
	{
		LogFailure(log, oldName, "replaced", GetLastError(),
		           "; settings kept in " + oldBare + ".imported, " + newBare + " untouched");
		return LEGACY_INI_FAILED;
	}
	err = WriteReplacing(newPath, in.oldText);
	if (err)
	{
		LogFailure(log, oldName, "write", err, "; settings kept in " + oldBare + ".imported");
		return LEGACY_INI_FAILED;
	}
	if (in.newExists)
		log("LegacyIni: " + oldBare + " imported into " + newBare + " (previous file kept as " + oldBare
		    + ".imported; previous " + newBare + " kept as " + newBare + ".replaced)");
	else
		log("LegacyIni: " + oldBare + " imported into " + newBare + " (previous file kept as " + oldBare
		    + ".imported)");
	return LEGACY_INI_IMPORT;
}
