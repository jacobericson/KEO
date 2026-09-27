// player_repath_tier.cpp -- publishes the player-owned HavokCharacter* set
// so hook_requestPath can tier a mid-walk re-request the same as the order
// that started it. See player_repath_tier.h.

#include "pathfind/player_repath_tier.h"


#include "base/core.h"
#include "game/game.h"
#include <windows.h>

namespace player_repath_tier_detail {

const int PRT_MAX_CHARS = 256;

// Classic single-buffer seqlock: odd while being written, even and stable
// once published. Two failed read attempts fall back to "no match" (the
// request keeps its game priority), never to a stale or torn array.
struct PlayerSet
{
	volatile LONG seq;
	int           count;
	uintptr_t     havokChars[PRT_MAX_CHARS];
};

PlayerSet g_playerSet = { 0, 0, {0} };

volatile long g_repathSeen      = 0;  // every priority<2 requestPath call examined
volatile long g_repathWouldTier = 0;  // of those, matched the published set
volatile long g_repathTiered    = 0;  // of those, the tier was actually written
volatile long g_repathSetSize   = 0;  // most recent publish's count (gauge)

} // namespace
using namespace player_repath_tier_detail;


namespace player_repath_tier_detail {

void PublishArray(const uintptr_t* local, int n)
{
	InterlockedIncrement(&g_playerSet.seq);   // odd: writing
	_ReadWriteBarrier();
	g_playerSet.count = n;
	for (int i = 0; i < n; ++i)
		g_playerSet.havokChars[i] = local[i];
	_ReadWriteBarrier();
	InterlockedIncrement(&g_playerSet.seq);   // even: consistent

	InterlockedExchange(&g_repathSetSize, n);
}

} // namespace
using namespace player_repath_tier_detail;

void PlayerRepathTierPublish(double /*now*/)
{
	uintptr_t playerIntf = *(uintptr_t*)((uintptr_t)GameAddr(RVA_GLOBAL_PLAYER));
	uintptr_t local[PRT_MAX_CHARS];
	int n = 0;

	if (playerIntf)
	{
		unsigned int count = GetPlayerCharCount(playerIntf);
		uintptr_t* stuff = GetPlayerCharStuff(playerIntf);
		if (stuff && count > 0 && count <= 200)
		{
			for (unsigned int i = 0; i < count && n < PRT_MAX_CHARS; ++i)
			{
				uintptr_t character = stuff[i];
				if (!character)
					continue;
				uintptr_t charMov = *(uintptr_t*)(KLIB_MEMBER(3, character, Character_movement, OFF_CHAR_MOVEMENT));
				if (!charMov)
					continue;
				uintptr_t havokChar = *(uintptr_t*)(KLIB_MEMBER(3, charMov, CharMovement_havokCharacter, OFF_CMOV_HAVOK_CHAR));
				if (havokChar)
					local[n++] = havokChar;
			}
		}
	}

	PublishArray(local, n);
}

void PlayerRepathTierClear()
{
	PublishArray(NULL, 0);
}

bool PlayerRepathTierIsPlayerOwned(uintptr_t havokChar)
{
	InterlockedIncrement(&g_repathSeen);
	if (!havokChar)
		return false;

	for (int attempt = 0; attempt < 2; ++attempt)
	{
		LONG seq1 = g_playerSet.seq;
		if (seq1 & 1)
			continue;
		_ReadWriteBarrier();
		int count = g_playerSet.count;
		bool found = false;
		for (int i = 0; i < count; ++i)
		{
			if (g_playerSet.havokChars[i] == havokChar)
			{
				found = true;
				break;
			}
		}
		_ReadWriteBarrier();
		LONG seq2 = g_playerSet.seq;
		if (seq1 != seq2)
			continue;
		if (found)
			InterlockedIncrement(&g_repathWouldTier);
		return found;
	}
	return false;
}

void PlayerRepathTierNoteTiered()
{
	InterlockedIncrement(&g_repathTiered);
}

void PlayerRepathTierAppendStats(std::ostringstream& ss)
{
	long seen    = InterlockedCompareExchange(&g_repathSeen, 0, 0);
	long would   = InterlockedCompareExchange(&g_repathWouldTier, 0, 0);
	long tiered  = InterlockedCompareExchange(&g_repathTiered, 0, 0);
	long setSize = InterlockedCompareExchange(&g_repathSetSize, 0, 0);

	ss << " playerRepath=seen" << seen << "/would" << would
	   << "/tiered" << tiered << "/set" << setSize;
}

bool PlayerRepathTierAnyStats()
{
	return InterlockedCompareExchange(&g_repathSeen, 0, 0) != 0;
}

