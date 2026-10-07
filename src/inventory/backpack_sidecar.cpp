// backpack_sidecar.cpp - The backpack-first table across a save and a load, and its re-key.
// The save writes the table into the save's temp folder before SaveFileSystem::saveGame queues
// the folder's copy, so the file travels with the save on every copy path; the load reads it
// back once the save is indexed and before any character exists; a new game clears the table.
// The re-key follows HandleManager's redirects, so a character moved to another squad keeps its
// entry. Everything here runs on the main thread, the table's only writer, and takes no lock of
// its own (writeFile takes SaveFileSystem's currentMutex inside the game).
#include "inventory/backpack_sidecar.h"
#include "inventory/backpack_sidecar_policy.h"
#include "inventory/backpack_table.h"
#include "inventory/backpack_first.h"
#include "inventory/backpack_window.h"
#include "inventory/backpack_food.h"
#include "inventory/byte_check_policy.h"
#include "inventory/inventory_config.h"
#include "game/hand_key.h"
#include "game/game.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <string>
#include <vector>

namespace backpack_sidecar_detail {
typedef char         (__fastcall *sfsSaveGame_t)(void* sfs, const std::string* savePath);
typedef void         (__fastcall *sfsLoadGame_t)(void* sfs, const std::string* savePath);
typedef void         (__fastcall *sfsNewGame_t)(void* sfs);
typedef std::string* (__fastcall *sfsPathOf_t)(void* sfs, std::string* retstr, const std::string* name);
typedef void*        (__fastcall *hmGetCharacter_t)(void* hm, const void* hand, bool deadOnes, bool redirect);
// The game's hand, as getCharacter reads it: its vftable (operator== at slot 0) and the five
// identity fields at +0x8..+0x18.
struct HandBlock { const void* vftable; unsigned type, container, containerSerial, index, serial, pad; };
} // namespace backpack_sidecar_detail
using namespace backpack_sidecar_detail;

static_assert(sizeof(HandBlock) == 0x20, "HandBlock is the game's 0x20-byte hand");

namespace keo_inventory {

// The callees' first 16 bytes, checked at install as callee heads before any of them is called.
static const unsigned char kWriteFileHead[16] =
	{ 0x40,0x55,0x56,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF };
static const unsigned char kReadFileHead[16] =
	{ 0x48,0x89,0x5C,0x24,0x10,0x55,0x56,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xDA,0x48 };
static const unsigned char kGetCharacterHead[16] =
	{ 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x55 };

static sfsSaveGame_t    orig_sfsSaveGame  = NULL;
static sfsLoadGame_t    orig_sfsLoadGame  = NULL;
static sfsNewGame_t     orig_sfsNewGame   = NULL;
static sfsPathOf_t      fn_sfsWriteFile   = NULL;
static sfsPathOf_t      fn_sfsReadFile    = NULL;
static hmGetCharacter_t fn_hmGetCharacter = NULL;
static void*            s_handleManager   = NULL;
static const void*      s_handVftable     = NULL;
// Set last, once all three hooks are in; until then every installed hook only calls its original.
static volatile LONG    s_armed           = 0;

// Main thread only.
static long s_rekeyed = 0, s_dropped = 0;
static long s_read = 0, s_missing = 0, s_refused = 0, s_badLines = 0, s_duplicates = 0, s_overflow = 0;
static long s_written = 0, s_unqueued = 0, s_writeFail = 0, s_formatFailed = 0;
static double s_nextRekey = 0.0;
static double s_nextBeat  = 0.0;
static const double kRekeySeconds = 1.0;
static const double kBeatSeconds  = 60.0;

// Main thread, from the saveGame hook. The game returns each path by constructing it into an
// empty std::string of ours (no heap block to leak), which this frame then destroys. True when
// the file was written whole; the caller counts it once saveGame has answered.
static bool SidecarWrite(void* sfs)
{
	try
	{
		BackpackRekeyNow(true);
		SidecarEntry entries[BACKPACK_TABLE_CAP];
		int n = 0;
		const int slots = BackpackFirstSlotCount();
		for (int i = 0; i < slots && n < BACKPACK_TABLE_CAP; ++i)
		{
			game::HandKey k;
			int on = 0;
			if (BackpackFirstEntry(i, &k, &on))
			{
				entries[n].key = k;
				entries[n].on = on;
				++n;
			}
		}
		int formatFailed = 0;
		std::string text = SidecarFormat(entries, n, &formatFailed);
		s_formatFailed += formatFailed;
		std::string name(SIDECAR_FILE_NAME); std::string path; fn_sfsWriteFile(sfs, &path, &name);
		if (path.empty())
		{
			++s_writeFail;
			return false;
		}
		FILE* f = NULL;
		if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
		{
			++s_writeFail;
			return false;
		}
		const size_t wrote = fwrite(text.data(), 1, text.size(), f);
		const int closed = fclose(f);
		if (wrote == text.size() && closed == 0)
			return true;
		++s_writeFail;
		return false;
	}
	catch (...)
	{
		++s_writeFail;
		return false;
	}
}

// Main thread, from the loadGame hook, after the save is indexed. A save without the file (one
// from before the sidecar) or a refused file leaves the table as newGame cleared it.
static void SidecarRead(void* sfs)
{
	try
	{
		std::string name(SIDECAR_FILE_NAME);
		std::string path;
		fn_sfsReadFile(sfs, &path, &name);
		if (path.empty())
		{
			++s_missing;
			return;
		}
		FILE* f = NULL;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
		{
			++s_missing;
			return;
		}
		// One byte past the cap, so an oversize file reaches the parser over the cap and is refused.
		std::vector<char> buf((size_t)SIDECAR_MAX_BYTES + 1);
		const size_t got = fread(&buf[0], 1, buf.size(), f);
		fclose(f);

		SidecarEntry entries[BACKPACK_TABLE_CAP];
		SidecarParseResult r = SidecarParse(&buf[0], (int)got, entries, BACKPACK_TABLE_CAP);
		if (r.refused)
		{
			++s_refused;
			return;
		}
		BackpackFirstClear();
		for (int i = 0; i < r.accepted; ++i)
			BackpackFirstSet(entries[i].key, entries[i].on);
		++s_read;
		s_badLines   += r.badLines;
		s_duplicates += r.duplicates;
		s_overflow   += r.overflow;
	}
	catch (...)
	{
		++s_refused;
	}
}

// Main thread, before the save's copy: the table re-keyed, unresolved entries dropped, the
// sidecar written into the temp folder. A written file counts as written only when saveGame
// queued the copy, else as unqueued. Never throws into the game.
static char __fastcall hook_sfsSaveGame(void* sfs, const std::string* savePath)
{
	const bool wrote = InterlockedCompareExchange(&s_armed, 0, 0) && SidecarWrite(sfs);
	const char r = orig_sfsSaveGame(sfs, savePath);
	if (wrote)
	{
		if (r)
			++s_written;
		else
			++s_unqueued;
	}
	return r;
}

// Main thread: the save is indexed and no character exists yet; the table is filled from the
// save's sidecar (newGame, inside loadGame, already cleared it).
static void __fastcall hook_sfsLoadGame(void* sfs, const std::string* savePath)
{
	orig_sfsLoadGame(sfs, savePath);
	if (InterlockedCompareExchange(&s_armed, 0, 0))
		SidecarRead(sfs);
}

// Main thread: a new game, a load or an import starts with no entries.
static void __fastcall hook_sfsNewGame(void* sfs)
{
	orig_sfsNewGame(sfs);
	if (InterlockedCompareExchange(&s_armed, 0, 0))
		BackpackFirstClear();
}

void InstallBackpackSidecar(int* installed, int*)
{
	if (!HookRowWanted(HOOK_SFS_SAVE_GAME)) return;
	const char* why = NULL;
	ByteRowLog rows;
	ByteRowLogReset(&rows);
	if (!ByteRowCheck(&rows, "writeFile", BYTE_CHECK_CALLEE_HEAD,
	                  (const unsigned char*)GameAddr(RVA_SFS_WRITE_FILE), kWriteFileHead, 16)
	 || !ByteRowCheck(&rows, "readFile", BYTE_CHECK_CALLEE_HEAD,
	                  (const unsigned char*)GameAddr(RVA_SFS_READ_FILE), kReadFileHead, 16)
	 || !ByteRowCheck(&rows, "getCharacter", BYTE_CHECK_CALLEE_HEAD,
	                  (const unsigned char*)GameAddr(RVA_HANDLE_MANAGER_GET_CHARACTER), kGetCharacterHead, 16))
		why = rows.why;
	if (!why)
	{
		fn_sfsWriteFile   = (sfsPathOf_t)GameAddr(RVA_SFS_WRITE_FILE);
		fn_sfsReadFile    = (sfsPathOf_t)GameAddr(RVA_SFS_READ_FILE);
		fn_hmGetCharacter = (hmGetCharacter_t)GameAddr(RVA_HANDLE_MANAGER_GET_CHARACTER);
		s_handleManager   = GameAddr(RVA_GLOBAL_HANDLE_MANAGER);
		s_handVftable     = GameAddr(RVA_HAND_VFTABLE);
		why = HookInstall(HOOK_SFS_NEW_GAME, hook_sfsNewGame, &orig_sfsNewGame, installed, true);
		if (!why)
			why = HookInstall(HOOK_SFS_LOAD_GAME, hook_sfsLoadGame, &orig_sfsLoadGame, installed, true);
		if (!why)
			why = HookInstall(HOOK_SFS_SAVE_GAME, hook_sfsSaveGame, &orig_sfsSaveGame, installed, true);
	}
	if (!why)
	{
		InterlockedExchange(&s_armed, 1);
		LogMsg(std::string("BackpackSidecar: installed shared=") + ByteRowShared(&rows));
	}
	else
	{
		LogError(std::string("BackpackSidecar: not installed (") + why + ")");
	}
}

bool BackpackResolveKey(const game::HandKey& k, game::HandKey* live)
{
	if (live)
		*live = k;
	if (!fn_hmGetCharacter || !s_handleManager || !s_handVftable)
		return false;
	HandBlock h = { s_handVftable, k.type, k.container, k.containerSerial, k.index, k.serial, 0 };
	void* c = fn_hmGetCharacter(s_handleManager, &h, false, true);
	if (!c)
		return false;
	if (live)
		*live = game::HandKeyOfObject(c);
	return true;
}

int BackpackRekeyNow(bool dropUnresolved)
{
	if (!fn_hmGetCharacter || !s_handleManager || !s_handVftable)
		return 0;
	int moved = 0;
	const int slots = BackpackFirstSlotCount();
	for (int i = 0; i < slots; ++i)
	{
		game::HandKey k;
		int on = 0;
		if (!BackpackFirstEntry(i, &k, &on))
			continue;
		HandBlock h = { s_handVftable, k.type, k.container, k.containerSerial, k.index, k.serial, 0 };
		void* c = fn_hmGetCharacter(s_handleManager, &h, false, true);
		if (!c)
		{
			if (dropUnresolved)
			{
				BackpackFirstErase(i);
				++s_dropped;
			}
			continue;
		}
		game::HandKey live = game::HandKeyOfObject(c);
		if (!game::HandKeyEqual(live, k))
		{
			BackpackFirstRekey(i, live);
			++moved;
		}
	}
	s_rekeyed += moved;
	return moved;
}

void BackpackRekeyTick(double now, bool saveLoading)
{
	if (InterlockedCompareExchange(&s_armed, 0, 0) && !saveLoading && now >= s_nextRekey)
	{
		s_nextRekey = now + kRekeySeconds;
		BackpackRekeyNow(false);
	}

	// Unconditional, on a timer: a sidecar that never armed and one that never had work must
	// read differently in a log.
	if (now < s_nextBeat)
		return;
	s_nextBeat = now + kBeatSeconds;
	long box[5];
	BackpackWindowCounters(box);
	std::ostringstream ss;
	ss << "Backpack: first=" << (BackpackFirstInstalled() ? 1 : 0)
	   << " sidecar=" << (InterlockedCompareExchange(&s_armed, 0, 0) ? 1 : 0)
	   << " entries=" << BackpackFirstEntryCount()
	   << " calls=" << BackpackFirstCalls()
	   << " routed=" << BackpackFirstRouted()
	   << " placed=" << BackpackFirstPlaced()
	   << " fellBack=" << BackpackFirstFellBack()
	   << " rekeyed=" << s_rekeyed
	   << " dropped=" << s_dropped
	   << " read=" << s_read
	   << " missing=" << s_missing
	   << " refused=" << s_refused
	   << " badLines=" << s_badLines
	   << " written=" << s_written
	   << " unqueued=" << s_unqueued
	   << " writeFail=" << s_writeFail
	   << " formatFailed=" << s_formatFailed
	   << " tableFull=" << BackpackFirstFullCount()
	   << " raced=" << BackpackFirstRaceCount()
	   << " duplicates=" << s_duplicates
	   << " overflow=" << s_overflow
	   << " boxes=" << box[0]
	   << " boxSkipped=" << box[1]
	   << " noArrange=" << box[2]
	   << " clicks=" << box[3]
	   << " clickFailed=" << box[4]
	   << " foodCalls=" << BackpackFoodCalls();
	LogMsg(ss.str());
}

} // namespace keo_inventory
