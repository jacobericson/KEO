#include <cstdio>
#include <cstring>
#include <string>
#include "diag/module_bases.h"

#include "check.h"

static ModuleBaseEntry MakeEntry(const char* name, unsigned __int64 base, unsigned __int64 size)
{
	ModuleBaseEntry e;
	e.base = base;
	e.size = size;
	size_t i = 0;
	for (; name[i] && i + 1 < sizeof(e.name); ++i)
		e.name[i] = name[i];
	e.name[i] = '\0';
	return e;
}

int main()
{
	ModuleBaseEntry mods[3];
	mods[0] = MakeEntry("exe", 0x140000000ULL, 0x2000000ULL);
	mods[1] = MakeEntry("RE_Kenshi.dll", 0x180000000ULL, 0x100000ULL);
	mods[2] = MakeEntry("KenshiZoneOpt.dll", 0x7FF700000000ULL, 0x50000ULL);

	char buf[512];
	int truncated = -1;
	size_t n = FormatModuleBases(buf, sizeof(buf), mods, 3, &truncated);
	Check(n > 0 && n < sizeof(buf), "formats into the buffer");
	Check(truncated == 0, "nothing left out when the buffer is roomy");
	Check(std::string(buf).find("Module bases:") == 0, "starts with the fixed marker");
	Check(std::string(buf).find("exe=0x140000000+0x2000000") != std::string::npos,
	      "the exe entry reads base+size in hex");
	Check(std::string(buf).find("RE_Kenshi.dll=0x180000000+0x100000") != std::string::npos,
	      "a second module is appended, not overwritten");

	// A cap too small for even the marker: writes nothing but still
	// NUL-terminates and reports every entry as left out.
	{
		char tiny[4];
		int trunc2 = -1;
		size_t n2 = FormatModuleBases(tiny, sizeof(tiny), mods, 3, &trunc2);
		Check(n2 < sizeof(tiny), "never writes past a tiny cap");
		Check(tiny[n2] == '\0', "still NUL-terminated at a tiny cap");
	}

	// A cap that fits the marker and the first entry but not the second:
	// the second is dropped whole, never emitted half-written.
	{
		char small[40];
		int trunc3 = -1;
		size_t n3 = FormatModuleBases(small, sizeof(small), mods, 3, &trunc3);
		std::string s(small, small + n3);
		Check(s.find("exe=") != std::string::npos, "the first entry that fits is kept");
		Check(s.find("RE_Kenshi") == std::string::npos,
		      "an entry that would not fully fit is not emitted partially");
		Check(trunc3 >= 1, "the dropped entries are counted, not silently lost");
		Check(small[n3] == '\0', "the truncated buffer is still NUL-terminated");
	}

	// Zero modules: still the marker, zero truncated, valid empty list.
	{
		char empty[32];
		int trunc4 = -1;
		size_t n4 = FormatModuleBases(empty, sizeof(empty), mods, 0, &trunc4);
		Check(std::string(empty, empty + n4) == "Module bases:", "no entries still prints the marker alone");
		Check(trunc4 == 0, "zero entries is not reported as truncation");
	}

	// Resolution: an address inside a module's [base, base+size) range.
	unsigned __int64 off = 0;
	int idx = ResolveModuleForAddress(mods, 3, 0x180000000ULL + 0x5460ULL, &off);
	Check(idx == 1, "an address inside RE_Kenshi.dll resolves to it");
	Check(off == 0x5460ULL, "the offset is addr - base");

	Check(ResolveModuleForAddress(mods, 3, 0x180000000ULL + 0x100000ULL, &off) == -1,
	      "the address one past the end of a module's range does not match it");
	Check(ResolveModuleForAddress(mods, 3, 0x180000000ULL - 1, &off) == -1,
	      "the address one before a module's base does not match it");

	ModuleBaseEntry zeroSize = MakeEntry("unreadable.dll", 0x190000000ULL, 0);
	Check(ResolveModuleForAddress(&zeroSize, 1, 0x190000000ULL + 4, &off) == -1,
	      "a module with an unread size never matches, rather than matching everything past its base");

	Check(ResolveModuleForAddress(mods, 3, 0x1ULL, &off) == -1,
	      "an address inside no known module resolves to nothing");

	// SelectModuleBases: priority entries survive a cap too small for every
	// module, and ordinary entries still fill whatever room is left.
	{
		ModuleBaseEntry all[5];
		all[0] = MakeEntry("a.dll", 0x1000ULL, 0x10ULL);
		all[1] = MakeEntry("RE_Kenshi.dll", 0x2000ULL, 0x10ULL);
		all[2] = MakeEntry("b.dll", 0x3000ULL, 0x10ULL);
		all[3] = MakeEntry("KenshiZoneOpt.dll", 0x4000ULL, 0x10ULL);
		all[4] = MakeEntry("c.dll", 0x5000ULL, 0x10ULL);

		unsigned __int64 priority[2] = { 0x4000ULL, 0x2000ULL }; // self, then RE_Kenshi

		// A cap that can only hold the two priority entries plus one more:
		// both priority bases must be present regardless of enumeration order.
		ModuleBaseEntry out[3];
		int n = SelectModuleBases(all, 5, priority, 2, out, 3);
		Check(n == 3, "fills exactly to the cap when more modules remain");
		bool haveSelf = false, haveReKenshi = false;
		for (int i = 0; i < n; ++i)
		{
			if (out[i].base == 0x4000ULL) haveSelf = true;
			if (out[i].base == 0x2000ULL) haveReKenshi = true;
		}
		Check(haveSelf, "the priority module (self) survives a too-small cap");
		Check(haveReKenshi, "the priority module (RE_Kenshi.dll) survives a too-small cap");
		Check(out[0].base == 0x4000ULL && out[1].base == 0x2000ULL,
		      "priority entries are written first, in priority order");

		// A priority base absent from the enumeration is simply skipped, not
		// invented and not counted against the cap.
		unsigned __int64 missing[1] = { 0x9999ULL };
		ModuleBaseEntry out2[5];
		int n2 = SelectModuleBases(all, 5, missing, 1, out2, 5);
		Check(n2 == 5, "an unmatched priority base costs no slot");

		// No priority bases at all: falls back to plain enumeration order.
		ModuleBaseEntry out3[5];
		int n3 = SelectModuleBases(all, 5, NULL, 0, out3, 5);
		Check(n3 == 5 && out3[0].base == 0x1000ULL,
		      "no priority bases keeps the original enumeration order");

		// A cap of zero writes nothing.
		ModuleBaseEntry out4[1];
		Check(SelectModuleBases(all, 5, priority, 2, out4, 0) == 0,
		      "a zero cap writes nothing");
	}

	// FormatAddrMod: the four distinct shapes the crash record's addrMod=
	// and ripMod= fields can take.
	{
		char buf[48];

		FormatAddrMod(buf, sizeof(buf), mods, 3, true, 0x180000000ULL + 0x5460ULL, 0);
		Check(std::string(buf) == "RE_Kenshi.dll+0x5460",
		      "an address inside a known module resolves to name+offset");

		FormatAddrMod(buf, sizeof(buf), mods, 3, true, 0x1ULL, 0);
		Check(std::string(buf) == "none",
		      "a real list with no match and no image base reports none");

		FormatAddrMod(buf, sizeof(buf), mods, 3, false, 0x1ULL, 0);
		Check(std::string(buf) == "?",
		      "haveList=false and no image base is ?, not a false none");

		FormatAddrMod(buf, sizeof(buf), NULL, 0, false, 0x1ULL, 0);
		Check(std::string(buf) == "?",
		      "no list at all (enumeration never ran) is still ?, not none");

		// A live VirtualQuery hit (imageBase != 0) names the address even
		// when the module missed the snapshot -- loaded late, or simply not
		// in `mods` at all -- and outranks both none and ?.
		FormatAddrMod(buf, sizeof(buf), mods, 3, true, 0x7000ULL + 0x20ULL, 0x7000ULL);
		Check(std::string(buf) == "image@0x7000+0x20",
		      "an image base with no snapshot match still resolves to an address");

		FormatAddrMod(buf, sizeof(buf), NULL, 0, false, 0x7000ULL + 0x20ULL, 0x7000ULL);
		Check(std::string(buf) == "image@0x7000+0x20",
		      "an image base resolves even with no list at all (enumeration never ran)");

		// A module the snapshot's own cap dropped (SelectModuleBases keeps
		// only `cap` of `all`) still resolves against the full, uncurated
		// list -- the exact gap addrMod= exists to close.
		ModuleBaseEntry all[50];
		for (int i = 0; i < 50; ++i)
		{
			char name[16];
			sprintf_s(name, sizeof(name), "m%d.dll", i);
			all[i] = MakeEntry(name, 0x10000ULL * (i + 1), 0x100ULL);
		}
		ModuleBaseEntry curated[48];
		int curatedCount = SelectModuleBases(all, 50, NULL, 0, curated, 48);
		Check(curatedCount == 48, "the curated table is capped below the full count");

		unsigned __int64 droppedAddr = all[49].base + 0x10ULL; // m49.dll, past the cap
		Check(ResolveModuleForAddress(curated, curatedCount, droppedAddr, NULL) == -1,
		      "the curated (capped) table cannot resolve the module it dropped");
		FormatAddrMod(buf, sizeof(buf), all, 50, true, droppedAddr, 0);
		Check(std::string(buf) == "m49.dll+0x10",
		      "the full, uncurated list resolves the same address the cap dropped");
	}

	return CheckExit("module_bases_units");
}
