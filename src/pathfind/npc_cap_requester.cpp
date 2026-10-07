// npc_cap_requester.cpp - the NpcCapWho: ring: one writer, the path thread, and one reader, the main
// thread. Each slot's sequence word is odd while the writer fills it; the reader drops a slot whose
// word moved. Names are read raw from the game's objects inside one guarded reader on the main
// thread, where the handle is resolved.
#include "pathfind/npc_cap_requester.h"

#ifdef KEO_DEBUG
#include "base/core.h"
#include "game/game.h"
#include "pathfind/player_task_policy.h"
#include "pathfind/pathfind_config.h"
#include <intrin.h>
#include <string.h>
#include <sstream>

namespace npc_cap_requester_detail {
struct CapSlot
{
	volatile LONG    seq;        // odd while the path thread writes the slot
	unsigned __int64 owner[4];   // the request's owner handle, 32 bytes
	int              player;
};

struct CapWho
{
	int  found;       // the handle still names a character
	int  fault;       // a read faulted
	char name[48];
	char faction[48];
	char race[48];
	int  task;        // the current action's task type; -1 none, PT_NA unreadable
	int  goal;        // the current goal's task type; the same
};
} // namespace npc_cap_requester_detail
using namespace npc_cap_requester_detail;

static const int    CAP_RING          = 64;      // a power of two
static const int    CAP_PRINT_MAX     = 8;       // characters named per line
static const size_t REQ_OWNER         = 0x08;    // HavokCharacterMessage::owner (hand, 32 bytes)
static const size_t CHAR_DISPLAY_NAME = 0x18;    // RootObjectBase::displayName (std::string)
static const size_t CHAR_FACTION      = 0x10;    // RootObjectBase::owner (Faction*)
static const size_t FACTION_NAME      = 0x1A8;   // Faction::name (std::string)
static const size_t RACE_GAMEDATA     = 0x40;    // RaceData::data (GameData*)
static const size_t GAMEDATA_NAME     = 0x28;    // GameData::name (std::string)
static const size_t STR_SIZE          = 16;      // std::string: the buffer or pointer, then size, capacity
static const size_t STR_CAPACITY      = 24;

static CapSlot       s_ring[CAP_RING];
static LONG          s_writeIdx = 0;   // path thread
static volatile LONG s_written = 0;    // slots published
static LONG          s_readIdx = 0;    // main thread

void NpcCapRequesterNote(const void* request, int player)
{
	if (!request)
		return;
	CapSlot& s = s_ring[s_writeIdx & (CAP_RING - 1)];
	InterlockedIncrement(&s.seq);
	memcpy(s.owner, (const char*)request + REQ_OWNER, sizeof(s.owner));
	s.player = player;
	InterlockedIncrement(&s.seq);
	++s_writeIdx;
	InterlockedExchange(&s_written, s_writeIdx);
}

static bool ReadSlot(CapSlot& s, unsigned __int64* owner, int* player)
{
	LONG before = InterlockedCompareExchange(&s.seq, 0, 0);
	if (before & 1)
		return false;
	memcpy(owner, s.owner, sizeof(s.owner));
	*player = s.player;
	_ReadWriteBarrier();
	return InterlockedCompareExchange(&s.seq, 0, 0) == before;
}

static void CopyGameString(uintptr_t str, char* out, size_t cap)
{
	out[0] = '\0';
	unsigned __int64 size = *(const unsigned __int64*)(str + STR_SIZE);
	unsigned __int64 capacity = *(const unsigned __int64*)(str + STR_CAPACITY);
	const char* text = capacity >= 16 ? *(const char* const*)str : (const char*)str;
	if (!text || size > 4096)
		return;
	size_t n = (size_t)size < cap - 1 ? (size_t)size : cap - 1;
	memcpy(out, text, n);
	out[n] = '\0';
	for (size_t i = 0; i < n; ++i)
	{
		if (out[i] == '"' || (unsigned char)out[i] < 0x20)
			out[i] = '_';
	}
}

// POD only (MSVC refuses __try beside objects that unwind): a character freed under the read ends
// the read, never the session.
static void ResolveWho(const unsigned __int64* owner, CapWho* w)
{
	w->found = 0;
	w->fault = 0;
	w->name[0] = w->faction[0] = w->race[0] = '\0';
	w->task = -1;
	w->goal = -1;
	GuardEnter();
	__try
	{
		uintptr_t ch = (uintptr_t)KlibSelectedCharacter(owner);
		if (ch)
		{
			w->found = 1;
			CopyGameString(ch + CHAR_DISPLAY_NAME, w->name, sizeof(w->name));
			uintptr_t faction = *(const uintptr_t*)(ch + CHAR_FACTION);
			if (faction)
				CopyGameString(faction + FACTION_NAME, w->faction, sizeof(w->faction));
			uintptr_t race = *(const uintptr_t*)(KLIB_MEMBER(3, ch, Character_myRace, 0x2E0));
			uintptr_t raceData = race ? *(const uintptr_t*)(race + RACE_GAMEDATA) : 0;
			if (raceData)
				CopyGameString(raceData + GAMEDATA_NAME, w->race, sizeof(w->race));
			uintptr_t body = *(const uintptr_t*)(KLIB_MEMBER(3, ch, Character_body, PT_OFF_CHAR_BODY));
			if (body)
				PT_TASKER_TYPE(*(const uintptr_t*)(KLIB_MEMBER(3, body, CharBody_currentAction, PT_OFF_BODY_ACTION)), w->task);
			uintptr_t ai = *(const uintptr_t*)(KLIB_MEMBER(3, ch, Character_ai, PT_OFF_CHAR_AI));
			uintptr_t ts = ai ? *(const uintptr_t*)(KLIB_MEMBER(3, ai, AI_taskSystemAI, PT_OFF_AI_TASKSYS)) : 0;
			if (ts)
				PT_GOAL_TYPE(*(const uintptr_t*)(KLIB_MEMBER(3, ts, OrdersReceiver_currentGoal_taskData, PT_OFF_TS_GOAL)), w->goal);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		w->fault = 1;
	}
	GuardLeave();
}

static void AppendType(std::ostringstream& ss, int v)
{
	if (v == PT_NA)
		ss << "-";
	else
		ss << v;
}

void NpcCapRequesterPrintLine()
{
	LONG written = InterlockedCompareExchange(&s_written, 0, 0);
	LONG n = written - s_readIdx;
	LONG dropped = 0;
	if (n > CAP_RING)
	{
		dropped = n - CAP_RING;
		s_readIdx = written - CAP_RING;
		n = CAP_RING;
	}
	unsigned __int64 owners[CAP_PRINT_MAX][4];
	int counts[CAP_PRINT_MAX];
	int players[CAP_PRINT_MAX];
	int distinct = 0, torn = 0, other = 0;
	for (LONG i = s_readIdx; i < written; ++i)
	{
		unsigned __int64 owner[4];
		int player = 0;
		if (!ReadSlot(s_ring[i & (CAP_RING - 1)], owner, &player))
		{
			++torn;
			continue;
		}
		int k = 0;
		while (k < distinct && memcmp(owners[k] + 1, owner + 1, 3 * sizeof(owner[0])) != 0)
			++k;
		if (k < distinct)
		{
			++counts[k];
			continue;
		}
		if (distinct == CAP_PRINT_MAX)
		{
			++other;
			continue;
		}
		memcpy(owners[distinct], owner, sizeof(owner));
		counts[distinct] = 1;
		players[distinct] = player;
		++distinct;
	}
	s_readIdx = written;

	std::ostringstream ss;
	ss << "NpcCapWho: n=" << n << " dropped=" << dropped << " torn=" << torn << " other=" << other;
	// The serving request, and with it the NPC label the memo covers and this ring's owner handle,
	// comes from the csFindPath detour, which installs only with pathfindDiag (cleared on a failed
	// install): without it every search reads unattributed and the ring stays empty.
	ss << " label=" << (pathfind::g_pathfindCfg.pathfindDiagEnabled ? "on" : "refused(pathfindDiag)");
	for (int k = 0; k < distinct; ++k)
	{
		CapWho w;
		ResolveWho(owners[k], &w);
		ss << " [x" << counts[k] << (players[k] ? " player" : "");
		if (w.fault)
			ss << " fault]";
		else if (!w.found)
			ss << " gone]";
		else
		{
			ss << " \"" << w.name << "\" faction=\"" << w.faction << "\" race=\"" << w.race << "\" task=";
			AppendType(ss, w.task);
			ss << " goal=";
			AppendType(ss, w.goal);
			ss << "]";
		}
	}
	LogMsg(ss.str());
}

#else

// Nothing of the ring exists outside a DEV build.
typedef int NpcCapRequesterNotInThisBuild;

#endif // KEO_DEBUG
