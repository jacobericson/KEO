#include <cstdio>
#include <string>
#include <vector>
#include "base/ini_text.h"

#include "check.h"

static bool BoolIs(const char* text, bool want)
{
	bool got = !want;
	return ParseBool(text, &got) && got == want;
}

static bool Rejects(const char* text)
{
	bool got = false;
	return !ParseBool(text, &got);
}

int main()
{
	// The spellings the INI template itself uses.
	Check(BoolIs("true", true),   "true");
	Check(BoolIs("false", false), "false");
	Check(BoolIs("1", true),      "1");
	Check(BoolIs("0", false),     "0");
	Check(BoolIs("yes", true),    "yes");
	Check(BoolIs("no", false),    "no");
	Check(BoolIs("on", true),     "on");
	Check(BoolIs("off", false),   "off");

	// A hand-edited file carries whatever capitalisation the editor typed, and
	// a value that fails to parse leaves the key at its default with only a log
	// line to show for it.
	Check(BoolIs("True", true),   "True");
	Check(BoolIs("TRUE", true),   "TRUE");
	Check(BoolIs("False", false), "False");
	Check(BoolIs("FALSE", false), "FALSE");
	Check(BoolIs("Yes", true),    "Yes");
	Check(BoolIs("Off", false),   "Off");
	Check(BoolIs("tRuE", true),   "tRuE");

	// Still not a bool.
	Check(Rejects(""),      "empty string rejected");
	Check(Rejects("maybe"), "maybe rejected");
	Check(Rejects("2"),     "2 rejected");
	Check(Rejects("-1"),    "-1 rejected");
	Check(Rejects("truthy"),"truthy rejected");

	// The caller's output is untouched when the value does not parse.
	{
		bool kept = true;
		ParseBool("nonsense", &kept);
		Check(kept == true, "a rejected value leaves the caller's bool alone");
	}

	// A bool with a named third position: the third word wins over the bool
	// parse, and a value that is neither leaves both outputs alone.
	{
		bool b = false, third = false;
		Check(ParseBoolOr("measure", "measure", &b, &third) && third,
		      "the third word is recognised");
		Check(ParseBoolOr("MeAsUrE", "measure", &b, &third) && third,
		      "the third word is case-insensitive");
		Check(ParseBoolOr("true", "measure", &b, &third) && !third && b,
		      "true still parses as a bool, not as the third position");
		Check(ParseBoolOr("off", "measure", &b, &third) && !third && !b,
		      "false still parses as a bool");

		bool keptB = true, keptThird = true;
		Check(!ParseBoolOr("maybe", "measure", &keptB, &keptThird),
		      "an unrelated word is rejected");
		Check(keptB && keptThird, "a rejected value leaves both outputs alone");

		Check(!ParseBoolOr("measure", 0, &b, &third),
		      "with no third word offered, only bools parse");
	}

	// SplitIniLine feeds ParseBool, so the value it hands over is already
	// trimmed; comments and section headers never reach it.
	{
		std::string k, v;
		Check(SplitIniLine("  reprioFast = True  ", &k, &v) && k == "reprioFast" && v == "True",
		      "a spaced line splits and trims");
		Check(BoolIs(v.c_str(), true), "the trimmed value parses");
		Check(!SplitIniLine("# reprioFast=true", &k, &v), "comment line ignored");
		Check(!SplitIniLine("[section]", &k, &v),         "section header ignored");
		Check(!SplitIniLine("reprioFast=", &k, &v),       "empty value ignored");
	}

	// Duplicate-key tracking: last-wins, one message per repeated key, a key
	// applied once is never reported.
	{
		std::vector<IniDupSeen> seen;
		IniNoteAppliedKey(seen, "navmeshGenConcurrency", 75);
		IniNoteAppliedKey(seen, "navmeshWorkerCount", 76);
		IniNoteAppliedKey(seen, "navmeshGenConcurrency", 80);
		Check(seen.size() == 2, "two distinct keys tracked, not three entries");

		const IniDupSeen* gen = 0;
		const IniDupSeen* worker = 0;
		for (size_t i = 0; i < seen.size(); ++i)
		{
			if (seen[i].key == "navmeshGenConcurrency") gen = &seen[i];
			if (seen[i].key == "navmeshWorkerCount")    worker = &seen[i];
		}
		Check(gen && gen->firstLine == 75 && gen->lastLine == 80, "repeated key keeps first and moves last");
		Check(worker && worker->firstLine == 76 && worker->lastLine == 76, "a key seen once has firstLine == lastLine");

		Check(gen && IniDupMessage(*gen) ==
		      "Config: duplicate key navmeshGenConcurrency at line 80 (first at line 75); using the last value",
		      "duplicate-key message text (a key repeated at lines 75 and 80)");

		// A third occurrence still yields one entry; the message (built once,
		// after the whole file is read) always names the final line.
		IniNoteAppliedKey(seen, "navmeshGenConcurrency", 90);
		for (size_t i = 0; i < seen.size(); ++i)
			if (seen[i].key == "navmeshGenConcurrency") gen = &seen[i];
		Check(gen && gen->firstLine == 75 && gen->lastLine == 90, "a third occurrence still moves lastLine, first unchanged");
	}

	return CheckExit("ini_text_units");
}
