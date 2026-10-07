// byte_check_policy.cpp - The inventory lane's install-time byte rows, pure. Any thread; no lock,
// no allocation.
#include "inventory/byte_check_policy.h"
#include "game/prologue_policy.h"
#include <string.h>

namespace keo_inventory {

ByteCheckVerdict ByteCheckJudge(ByteCheckKind kind, const unsigned char* actual,
                                const unsigned char* expect, int len)
{
	if (!actual || !expect || len <= 0)
		return BYTE_CHECK_REFUSED;
	if (kind == BYTE_CHECK_EXACT)
		return memcmp(actual, expect, (size_t)len) == 0 ? BYTE_CHECK_ORIGINAL : BYTE_CHECK_REFUSED;
	// The shared-site rule reads exactly 16 bytes.
	if (kind != BYTE_CHECK_CALLEE_HEAD || len != 16)
		return BYTE_CHECK_REFUSED;
	switch (ClassifyPrologue(actual, expect, true))
	{
	case PROLOGUE_ORIGINAL: return BYTE_CHECK_ORIGINAL;
	case PROLOGUE_SHARED:   return BYTE_CHECK_SHARED;
	default:                return BYTE_CHECK_REFUSED;
	}
}

int ByteReadFormat(const char* name, const unsigned char* bytes, int len, char* out, int n)
{
	static const char kHex[] = "0123456789ABCDEF";
	if (!name || !bytes || !out || n <= 0)
		return 0;
	if (len > 16)
		len = 16;
	if (len < 0)
		len = 0;
	const int nameLen = (int)strlen(name);
	// " read=" and three characters a byte, less the space before the first.
	const int need = nameLen + 6 + (len > 0 ? len * 3 - 1 : 0);
	if (need + 1 > n)
		return 0;
	int w = 0;
	memcpy(out, name, (size_t)nameLen);
	w += nameLen;
	memcpy(out + w, " read=", 6);
	w += 6;
	for (int i = 0; i < len; ++i)
	{
		if (i > 0)
			out[w++] = ' ';
		out[w++] = kHex[bytes[i] >> 4];
		out[w++] = kHex[bytes[i] & 0xF];
	}
	out[w] = 0;
	return w;
}

void ByteRowLogReset(ByteRowLog* log)
{
	if (!log)
		return;
	log->why[0] = 0;
	log->shared[0] = 0;
	log->sharedLen = 0;
}

bool ByteRowCheck(ByteRowLog* log, const char* name, ByteCheckKind kind,
                  const unsigned char* actual, const unsigned char* expect, int len)
{
	const ByteCheckVerdict v = ByteCheckJudge(kind, actual, expect, len);
	if (!log || !name)
		return v != BYTE_CHECK_REFUSED;
	if (v == BYTE_CHECK_REFUSED)
	{
		log->shared[0] = 0;
		log->sharedLen = 0;
		if (!actual || ByteReadFormat(name, actual, len, log->why, (int)sizeof(log->why)) == 0)
		{
			size_t n = strlen(name);
			if (n > sizeof(log->why) - 1)
				n = sizeof(log->why) - 1;
			memcpy(log->why, name, n);
			log->why[n] = 0;
		}
		return false;
	}
	if (v == BYTE_CHECK_SHARED)
	{
		const size_t nameLen = strlen(name);
		const size_t used = (size_t)log->sharedLen;
		const size_t sep = used > 0 ? 1 : 0;
		if (used + sep + nameLen < sizeof(log->shared))
		{
			if (sep)
				log->shared[used] = ',';
			memcpy(log->shared + used + sep, name, nameLen);
			log->sharedLen = (int)(used + sep + nameLen);
			log->shared[log->sharedLen] = 0;
		}
	}
	return true;
}

const char* ByteRowShared(const ByteRowLog* log)
{
	return log && log->sharedLen > 0 ? log->shared : "none";
}

} // namespace keo_inventory
