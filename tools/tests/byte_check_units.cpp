// The inventory lane's install-time byte rows: the judgement of an exact row and of a callee head
// (original, behind an E9 or an FF 25 detour, a changed tail, the wrong kind or length), the
// refusal text naming the bytes read, and one install's record of refusals and shared heads.
#include <cstdio>
#include <cstring>
#include "inventory/byte_check_policy.h"

#include "check.h"

using namespace keo_inventory;

// The reader's two callee heads as this build carries them.
static const unsigned char kSectionHead[16] =
	{ 0x44,0x8B,0x49,0x70,0x45,0x33,0xC0,0x45,0x85,0xC9,0x74,0x1E,0x4C,0x8B,0x51,0x78 };
static const unsigned char kLimitedHead[16] =
	{ 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48 };

static void CheckJudge()
{
	// Arguments: kind, actual, expect, len.
	CHECK(ByteCheckJudge(BYTE_CHECK_EXACT, kSectionHead, kSectionHead, 16) == BYTE_CHECK_ORIGINAL
	      && ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, kLimitedHead, kLimitedHead, 16) == BYTE_CHECK_ORIGINAL,
	      "bytes: the bytes this build carries pass as original on either kind");
	unsigned char one[16];
	memcpy(one, kSectionHead, 16);
	one[9] ^= 0x01;
	CHECK(ByteCheckJudge(BYTE_CHECK_EXACT, one, kSectionHead, 16) == BYTE_CHECK_REFUSED
	      && ByteCheckJudge(BYTE_CHECK_EXACT, one, kSectionHead, 6) == BYTE_CHECK_ORIGINAL,
	      "bytes: an exact row refuses one changed byte inside its length only");
	// Another plugin's E9 over the first instruction (5 bytes): the tail matches from offset 5.
	static const unsigned char e9[16] =
		{ 0xE9,0x2B,0x10,0xF3,0xFF,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48 };
	CHECK(ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, e9, kLimitedHead, 16) == BYTE_CHECK_SHARED,
	      "bytes: a callee head behind an E9 detour with a matching tail is shared");
	// FF 25 rel32 (6 bytes) and padding to offset 8, the rest the function's own bytes.
	static const unsigned char ff25[16] =
		{ 0xFF,0x25,0x00,0x10,0x00,0x00,0x90,0xCC,0x85,0xC9,0x74,0x1E,0x4C,0x8B,0x51,0x78 };
	CHECK(ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, ff25, kSectionHead, 16) == BYTE_CHECK_SHARED,
	      "bytes: a callee head behind an FF 25 detour padded to offset 8 is shared");
	unsigned char tail[16];
	memcpy(tail, e9, 16);
	tail[12] ^= 0x01;
	CHECK(ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, tail, kLimitedHead, 16) == BYTE_CHECK_REFUSED,
	      "bytes: a detoured callee head whose tail differs refuses");
	CHECK(ByteCheckJudge(BYTE_CHECK_EXACT, e9, kLimitedHead, 16) == BYTE_CHECK_REFUSED
	      && ByteCheckJudge(BYTE_CHECK_EXACT, e9, kLimitedHead, 6) == BYTE_CHECK_REFUSED,
	      "bytes: an E9 head on an exact row refuses");
	CHECK(ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, kLimitedHead, kLimitedHead, 15) == BYTE_CHECK_REFUSED
	      && ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, e9, kLimitedHead, 6) == BYTE_CHECK_REFUSED,
	      "bytes: a callee row of any length but 16 refuses");
	CHECK(ByteCheckJudge(BYTE_CHECK_EXACT, NULL, kLimitedHead, 16) == BYTE_CHECK_REFUSED
	      && ByteCheckJudge(BYTE_CHECK_CALLEE_HEAD, e9, NULL, 16) == BYTE_CHECK_REFUSED
	      && ByteCheckJudge(BYTE_CHECK_EXACT, e9, e9, 0) == BYTE_CHECK_REFUSED,
	      "bytes: a missing pointer or an empty row refuses");

	char out[96];
	CHECK(ByteReadFormat("isEmpty", e9, 3, out, (int)sizeof(out)) == 21
	      && strcmp(out, "isEmpty read=E9 2B 10") == 0,
	      "bytes: the refusal text names the row and the bytes read");
	CHECK(ByteReadFormat("isLimitedSlotCompatible", e9, 16, out, (int)sizeof(out)) == 76
	      && strcmp(out, "isLimitedSlotCompatible read=E9 2B 10 F3 FF 48 89 74 24 10 57 48 83 EC 20 48") == 0
	      && ByteReadFormat("isLimitedSlotCompatible", e9, 40, out, (int)sizeof(out)) == 76,
	      "bytes: the refusal text carries at most 16 bytes");
	CHECK(ByteReadFormat("isEmpty", e9, 3, out, 22) == 21 && ByteReadFormat("isEmpty", e9, 3, out, 21) == 0,
	      "bytes: the refusal text needs room for its terminator");
}

static void CheckRows()
{
	ByteRowLog log;
	ByteRowLogReset(&log);
	CHECK(log.why[0] == 0 && strcmp(ByteRowShared(&log), "none") == 0,
	      "rows: a reset log has no refusal and reads shared=none");
	CHECK(ByteRowCheck(&log, "getSectionOfType", BYTE_CHECK_CALLEE_HEAD, kSectionHead, kSectionHead, 16)
	      && strcmp(ByteRowShared(&log), "none") == 0,
	      "rows: an original callee head passes and is not shared");
	static const unsigned char e9[16] =
		{ 0xE9,0x2B,0x10,0xF3,0xFF,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48 };
	static const unsigned char ff25[16] =
		{ 0xFF,0x25,0x00,0x10,0x00,0x00,0x90,0xCC,0x85,0xC9,0x74,0x1E,0x4C,0x8B,0x51,0x78 };
	CHECK(ByteRowCheck(&log, "isLimitedSlotCompatible", BYTE_CHECK_CALLEE_HEAD, e9, kLimitedHead, 16)
	      && ByteRowCheck(&log, "getSectionOfType", BYTE_CHECK_CALLEE_HEAD, ff25, kSectionHead, 16)
	      && strcmp(ByteRowShared(&log), "isLimitedSlotCompatible,getSectionOfType") == 0,
	      "rows: shared callee heads are listed in row order, comma-separated");
	CHECK(ByteRowCheck(&log, "limitedSlot", BYTE_CHECK_EXACT, kSectionHead, kSectionHead, 6)
	      && strcmp(ByteRowShared(&log), "isLimitedSlotCompatible,getSectionOfType") == 0,
	      "rows: a matching exact row passes and adds nothing to the shared list");
	CHECK(!ByteRowCheck(&log, "isEmpty", BYTE_CHECK_EXACT, e9, kLimitedHead, 9)
	      && strcmp(log.why, "isEmpty read=E9 2B 10 F3 FF 48 89 74 24") == 0
	      && strcmp(ByteRowShared(&log), "none") == 0,
	      "rows: a refusal names the row and its bytes and empties the shared list");
	ByteRowLogReset(&log);
	// A name longer than the refusal buffer itself: neither the bytes nor the whole name fit.
	char longName[160];
	memset(longName, 'a', sizeof(longName) - 1);
	longName[sizeof(longName) - 1] = 0;
	CHECK(!ByteRowCheck(&log, longName, BYTE_CHECK_CALLEE_HEAD, e9, kSectionHead, 16)
	      && strlen(log.why) == sizeof(log.why) - 1 && strncmp(log.why, longName, sizeof(log.why) - 1) == 0,
	      "rows: a refusal whose bytes do not fit keeps the row name, cut to fit");
	ByteRowLogReset(&log);
	for (int i = 0; i < 12; ++i)
		ByteRowCheck(&log, "isLimitedSlotCompatible", BYTE_CHECK_CALLEE_HEAD, e9, kLimitedHead, 16);
	CHECK(log.sharedLen < (int)sizeof(log.shared) && log.shared[log.sharedLen] == 0
	      && (int)strlen(log.shared) == log.sharedLen && log.sharedLen % 24 == 23,
	      "rows: the shared list never overruns and keeps only whole names");
}

int main()
{
	CheckJudge();
	CheckRows();
	return CheckExit("byte_check_units");
}
