// The navmesh cache's key hashes, run as the cache runs them, against fixed
// inputs whose values name every cached mesh; the mod-set hash loop; and
// base/hash.h against a reference FNV-1a loop.
// ModSetHash and the three settings byte tables in main() are transcriptions
// of config.cpp's ComputeModSetHash loop and nm_quality.h's
// NM_MATERIAL_SLOPE_BITS, NM_PRUNE_VANILLA and NM_EXTRA_VERTEX_VANILLA, not
// the originals: the literal expected hashes pin the copies, so an edit to an
// original is not seen here.

#include <string.h>
#include "navmesh/cache/nm_key_hash.h"
#include "base/hash.h"

#include "check.h"

namespace hash_units_detail
{

// FNV-1a over a byte range, the loop the cache's hashes are built from.
static unsigned int RefFnvBytes(unsigned int h, const void* data, size_t len)
{
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static void PutLe32(unsigned char* out, unsigned int v)
{
	out[0] = (unsigned char)(v & 0xFF);
	out[1] = (unsigned char)((v >> 8) & 0xFF);
	out[2] = (unsigned char)((v >> 16) & 0xFF);
	out[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void PutWords(unsigned char* out, const unsigned int* words, int n)
{
	for (int i = 0; i < n; ++i)
		PutLe32(out + 4 * i, words[i]);
}

// The mod-set loop: '\r' skipped, and a result of 0 reported as 1.
static unsigned int ModSetHash(const char* text)
{
	const unsigned char* buf = (const unsigned char*)text;
	size_t got = strlen(text);
	unsigned int h = FNV1A32_OFFSET;
	for (size_t i = 0; i < got; ++i)
	{
		if (buf[i] == '\r') continue;
		h = Fnv1a32Mix(h, buf[i]);
	}
	if (h == 0) h = 1;
	return h;
}

}
using namespace hash_units_detail;

int main()
{
	unsigned char slope[20];
	const unsigned int slopeWords[5] = { 0x3F860A92u, 0x3F860A92u, 0x3FC8F5C3u, 0x3F860A92u, 0x3F32B8C3u };
	PutWords(slope, slopeWords, 5);

	unsigned char prune[16];
	const unsigned int pruneWords[3] = { 0x4CBEBC20u, 0x3ECCCCCDu, 0u };
	PutWords(prune, pruneWords, 3);
	prune[12] = 0x00; prune[13] = 0x01; prune[14] = 0x00; prune[15] = 0x00;

	unsigned char xvert[40];
	const unsigned int xvertWords[10] = {
		0u, 0u, 0u, 0x447A0000u, 20u, 0x3D4CCCCDu, 0u, 0x42480000u, 0x42480000u, 0x3A83126Fu };
	PutWords(xvert, xvertWords, 10);

	Check(L2SettingsHashOf(slope, sizeof(slope), prune, sizeof(prune), xvert, sizeof(xvert),
	                       false, false) == 0xD3FE8BD2u,
	      "settings vanilla");
	Check(L2SettingsHashOf(slope, sizeof(slope), prune, sizeof(prune), xvert, sizeof(xvert),
	                       true, false) == 0x5F777F1Du,
	      "settings prune");
	Check(L2SettingsHashOf(slope, sizeof(slope), prune, sizeof(prune), xvert, sizeof(xvert),
	                       true, true) == 0x8F42260Bu,
	      "settings nbrseed");

	const float aabb[6] = { -1.5f, 0.0f, 2.25f, 100.0f, 50.5f, 300.0f };
	Check(NmAabbHash(aabb) == 0x3D965A3Du, "aabb");

	const float pos[3] = { 10.0f, 20.0f, 30.0f };
	const float rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const char sid[] = "12345-rf_building.mod";
	Check(NmBuildingHashOf(pos, rot, sid, sizeof(sid) - 1) == 0xBEEEC354u, "building");
	Check(NmBuildingHashOf(pos, rot, NULL, 0) == 0xF7BC446Bu, "building (no stringID)");

	Check(ModSetHash("gamedata.base\r\nNewland.mod\r\n") == 0xF5FD096Du, "modset");

	const unsigned char none = 0;
	Check(Fnv1a32(&none, 0) == 0x811C9DC5u, "empty");

	bool foldOk = true;
	for (int n = 0; n < 64; ++n)
	{
		unsigned char buf[64];
		for (int i = 0; i < n; ++i)
			buf[i] = (unsigned char)(i * 37 + 11);
		if (RefFnvBytes(2166136261u, buf, (size_t)n) != Fnv1a32Bytes(FNV1A32_OFFSET, buf, (size_t)n))
			foldOk = false;
	}
	Check(foldOk, "fold");

	return CheckExit("hash_units");
}
