// The backpack-first sidecar's text: the parser's refusals, skips and counts line by line, the
// format's version line and default filter, and a format-then-parse round trip, whole and cut.
#include <cstdio>
#include <cstring>
#include <string>
#include "inventory/backpack_sidecar_policy.h"

#include "check.h"

using game::HandKey;
using namespace keo_inventory;

static HandKey Key(unsigned type, unsigned container, unsigned containerSerial, unsigned index, unsigned serial)
{
	HandKey k = { type, container, containerSerial, index, serial };
	return k;
}

static SidecarEntry Entry(const HandKey& k, int on)
{
	SidecarEntry e = { k, on };
	return e;
}

static SidecarParseResult Parse(const std::string& text, SidecarEntry* out, int max)
{
	return SidecarParse(text.c_str(), (int)text.size(), out, max);
}

static bool Counts(const SidecarParseResult& r, bool refused, int accepted, int badLines, int duplicates, int overflow)
{
	return r.refused == refused && r.accepted == accepted && r.badLines == badLines
	    && r.duplicates == duplicates && r.overflow == overflow;
}

static bool Holds(const SidecarEntry& e, const HandKey& k, int on)
{
	return game::HandKeyEqual(e.key, k) && e.on == on;
}

static void CheckParseRefusals()
{
	SidecarEntry out[8];
	CHECK(Counts(SidecarParse("", 0, out, 8), false, 0, 0, 0, 0)
	      && Counts(SidecarParse("KEO_backpacks 1\n", 0, out, 8), false, 0, 0, 0, 0),
	      "parse: an empty text is no file");
	CHECK(Counts(Parse("2 7 31 405 1 0\n", out, 8), true, 0, 0, 0, 0)
	      && Counts(Parse("\n# a comment\n\n", out, 8), true, 0, 0, 0, 0)
	      && Counts(Parse("KEO_backpacks\n2 7 31 405 1 0\n", out, 8), true, 0, 0, 0, 0),
	      "parse: a missing version line refuses the file");
	CHECK(Counts(Parse("KEO_backpacks 2\n2 7 31 405 1 0\n", out, 8), true, 0, 0, 0, 0)
	      && Counts(Parse("KEO_backpacks 1 1\n2 7 31 405 1 0\n", out, 8), true, 0, 0, 0, 0)
	      && Counts(Parse("KEO_backpacks 10\n", out, 8), true, 0, 0, 0, 0),
	      "parse: another version refuses the file");
}

static void CheckParseLines()
{
	const HandKey a = Key(2, 7, 31, 405, 1);
	const HandKey b = Key(2, 7, 31, 406, 2);
	SidecarEntry out[8];

	SidecarParseResult r = Parse("KEO_backpacks 1\n2 7 31 405 1 0\n2 7 31 406\n2 7 31 406 2 1\n", out, 8);
	CHECK(Counts(r, false, 2, 1, 0, 0) && Holds(out[0], a, 0) && Holds(out[1], b, 1),
	      "parse: a short line is skipped and counted");

	r = Parse("KEO_backpacks 1\n2 7 x1 405 1 0\n2 7 31 405 1 -1\n2 7 31 406 2 1\n", out, 8);
	CHECK(Counts(r, false, 1, 2, 0, 0) && Holds(out[0], b, 1),
	      "parse: a non-numeric field is skipped and counted");

	r = Parse("KEO_backpacks 1\n2 7 31 405 1 2\n2 7 31 405 1 10\n2 7 31 406 2 0\n", out, 8);
	CHECK(Counts(r, false, 1, 2, 0, 0) && Holds(out[0], b, 0),
	      "parse: an on value other than 0 or 1 is skipped");

	r = Parse("KEO_backpacks 1\n2 7 31 405 1 0 9\n2 7 31 405 1 0 9 9 9\n2 7 31 406 2 1\n", out, 8);
	CHECK(Counts(r, false, 1, 2, 0, 0) && Holds(out[0], b, 1),
	      "parse: a line with seven fields is skipped");

	r = Parse("KEO_backpacks 1\n11 0 0 0 0 1\n11 7 31 405 1 0\n2 7 31 406 2 1\n", out, 8);
	CHECK(Counts(r, false, 1, 2, 0, 0) && Holds(out[0], b, 1),
	      "parse: a null key is skipped");

	r = Parse("KEO_backpacks 1\n2 7 31 405 1 1\n2 7 31 406 2 1\n2 7 31 405 1 0\n", out, 8);
	CHECK(Counts(r, false, 2, 0, 1, 0) && Holds(out[0], a, 0) && Holds(out[1], b, 1),
	      "parse: a duplicate key keeps the last line");

	r = Parse("KEO_backpacks 1\r\n2 7 31 405 1 1\r\n2 7 31 406 2 0\r\n", out, 8);
	CHECK(Counts(r, false, 2, 0, 0, 0) && Holds(out[0], a, 1) && Holds(out[1], b, 0),
	      "parse: CRLF line ends are read");

	r = Parse("KEO_backpacks 1\n2 7 31 405 1 1\n2 7 31 406 2 0", out, 8);
	SidecarParseResult r2 = Parse("KEO_backpacks 1\r\n2 7 31 405 1 1\r", out + 4, 4);
	CHECK(Counts(r, false, 2, 0, 0, 0) && Holds(out[1], b, 0)
	      && Counts(r2, false, 1, 0, 0, 0) && Holds(out[4], a, 1),
	      "parse: a last line without a newline is read");

	r = Parse("\n# header next\n  \t\nKEO_backpacks 1\n\n#2 7 31 405 1 1\n\t \n2 7 31 406 2 1\n\n", out, 8);
	CHECK(Counts(r, false, 1, 0, 0, 0) && Holds(out[0], b, 1),
	      "parse: blank and comment lines are skipped");

	r = Parse("KEO_backpacks 1\n2 7 31 405 1 1\n2 7 31 406 2 1\n2 7 31 407 3 0\n2 7 31 408 4 0\n2 7 31 406 2 0\n",
	          out, 2);
	CHECK(Counts(r, false, 2, 0, 1, 2) && Holds(out[0], a, 1) && Holds(out[1], b, 0),
	      "parse: lines past max are counted, not written");
}

static void CheckFormat()
{
	CHECK(SidecarFormat(NULL, 0, true) == "KEO_backpacks 1\n" && SidecarFormat(NULL, 0, false) == "KEO_backpacks 1\n",
	      "format: an empty table writes the version line alone");

	SidecarEntry e[3] = { Entry(Key(2, 7, 31, 405, 1), 1), Entry(Key(2, 7, 31, 406, 2), 0),
	                      Entry(Key(2, 8, 31, 405, 3), 1) };
	CHECK(SidecarFormat(e, 3, true) == "KEO_backpacks 1\n2 7 31 406 2 0\n"
	      && SidecarFormat(e, 3, false) == "KEO_backpacks 1\n2 7 31 405 1 1\n2 8 31 405 3 1\n"
	      && SidecarFormat(e, 1, true) == "KEO_backpacks 1\n",
	      "format: an entry equal to the default is not written");

	SidecarEntry order[3] = { Entry(Key(2, 9, 1, 900, 9), 0), Entry(Key(2, 1, 1, 100, 1), 0),
	                          Entry(Key(2, 5, 1, 500, 5), 0) };
	CHECK(SidecarFormat(order, 3, true) == "KEO_backpacks 1\n2 9 1 900 9 0\n2 1 1 100 1 0\n2 5 1 500 5 0\n",
	      "format: lines keep the given order");
}

static void CheckRoundTrip()
{
	SidecarEntry in[4] = { Entry(Key(2, 7, 31, 405, 1), 0), Entry(Key(2, 7, 31, 406, 4294967295u), 0),
	                       Entry(Key(4294967295u, 0, 0, 0, 0), 0), Entry(Key(2, 3, 4, 5, 6), 0) };
	std::string text = SidecarFormat(in, 4, true);
	SidecarEntry out[8];
	SidecarParseResult r = Parse(text, out, 8);
	bool same = Counts(r, false, 4, 0, 0, 0);
	for (int i = 0; same && i < 4; ++i)
		same = Holds(out[i], in[i].key, 0);
	CHECK(same, "round trip: format then parse gives the written entries");

	// Every cut inside the last line leaves fewer than six fields, so only that line is lost.
	size_t lastStart = text.rfind('\n', text.size() - 2) + 1;
	bool kept = true;
	for (size_t cut = lastStart + 1; kept && cut < text.size() - 1; ++cut)
	{
		SidecarParseResult c = SidecarParse(text.c_str(), (int)cut, out, 8);
		kept = Counts(c, false, 3, 1, 0, 0);
		for (int i = 0; kept && i < 3; ++i)
			kept = Holds(out[i], in[i].key, 0);
	}
	SidecarParseResult atLineEnd = SidecarParse(text.c_str(), (int)lastStart, out, 8);
	CHECK(kept && Counts(atLineEnd, false, 3, 0, 0, 0),
	      "round trip: a truncated file keeps every whole line before the cut");
}

int main()
{
	CheckParseRefusals();
	CheckParseLines();
	CheckFormat();
	CheckRoundTrip();
	return CheckExit("backpack_sidecar_units");
}
