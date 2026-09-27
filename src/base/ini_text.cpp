#include "base/ini_text.h"
#include <cstdlib>
#include <cstring>
#include <sstream>

std::string IniTrim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return std::string();
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

bool SplitIniLine(const std::string& raw, std::string* key, std::string* val)
{
	std::string line = IniTrim(raw);
	if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
		return false;
	size_t eq = line.find('=');
	if (eq == std::string::npos)
		return false;
	*key = IniTrim(line.substr(0, eq));
	*val = IniTrim(line.substr(eq + 1));
	return !key->empty() && !val->empty();
}

// Hand-written lowering: tolower() is locale-dependent, and a hand-edited
// INI carrying "True" must mean the same as "true".
static std::string IniLower(const std::string& val)
{
	std::string v(val);
	for (size_t i = 0; i < v.size(); ++i)
	{
		if (v[i] >= 'A' && v[i] <= 'Z')
			v[i] = (char)(v[i] - 'A' + 'a');
	}
	return v;
}

bool ParseBool(const std::string& val, bool* out)
{
	std::string v = IniLower(val);

	if (v == "true" || v == "1" || v == "yes" || v == "on")
		{ *out = true; return true; }
	if (v == "false" || v == "0" || v == "no" || v == "off")
		{ *out = false; return true; }
	return false;
}

bool ParseBoolOr(const std::string& val, const char* thirdWord,
                 bool* outBool, bool* outIsThird)
{
	if (thirdWord && IniLower(val) == IniLower(thirdWord))
		{ *outIsThird = true; return true; }
	if (ParseBool(val, outBool))
		{ *outIsThird = false; return true; }
	return false;
}

bool ParseInt(const std::string& val, int* out)
{
	char* end = NULL;
	long l = strtol(val.c_str(), &end, 10);
	if (end == val.c_str()) return false;
	*out = (int)l;
	return true;
}

bool ParseFloat(const std::string& val, float* out)
{
	char* end = NULL;
	float f = (float)strtod(val.c_str(), &end);
	if (end == val.c_str()) return false;
	*out = f;
	return true;
}

bool IniValueEquals(IniValueKind kind, const std::string& existing, const std::string& value)
{
	switch (kind)
	{
	case INI_BOOL:
	{
		bool a, b;
		return ParseBool(existing, &a) && ParseBool(value, &b) && a == b;
	}
	case INI_INT:
	{
		int a, b;
		return ParseInt(existing, &a) && ParseInt(value, &b) && a == b;
	}
	case INI_FLOAT:
	{
		float a, b;
		return ParseFloat(existing, &a) && ParseFloat(value, &b) && memcmp(&a, &b, sizeof(float)) == 0;
	}
	default:
		return existing == value;
	}
}

void IniNoteAppliedKey(std::vector<IniDupSeen>& seen, const std::string& key, int line)
{
	for (size_t i = 0; i < seen.size(); ++i)
	{
		if (seen[i].key == key)
		{
			seen[i].lastLine = line;
			return;
		}
	}
	IniDupSeen s;
	s.key = key;
	s.firstLine = line;
	s.lastLine = line;
	seen.push_back(s);
}

std::string IniDupMessage(const IniDupSeen& seen)
{
	std::ostringstream ss;
	ss << "Config: duplicate key " << seen.key << " at line " << seen.lastLine
	   << " (first at line " << seen.firstLine << "); using the last value";
	return ss.str();
}

struct IniLine
{
	std::string body;
	std::string eol;   // "\r\n", "\n", or empty on a last line without one
};

static void SplitLines(const std::string& text, std::vector<IniLine>* lines)
{
	size_t pos = 0;
	while (pos < text.size())
	{
		IniLine l;
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
		{
			l.body = text.substr(pos);
			pos = text.size();
		}
		else
		{
			bool crlf = nl > pos && text[nl - 1] == '\r';
			l.body = text.substr(pos, nl - pos - (crlf ? 1 : 0));
			l.eol = crlf ? "\r\n" : "\n";
			pos = nl + 1;
		}
		lines->push_back(l);
	}
}

static bool SameHeader(const std::string& trimmed, const char* header)
{
	if (trimmed.size() != strlen(header))
		return false;
	for (size_t i = 0; i < trimmed.size(); ++i)
	{
		char a = trimmed[i], b = header[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

// Swaps the value of a line SplitIniLine accepted, keeping everything
// around it: the key and its spacing, the spacing after '=', trailing blanks.
static void ReplaceValue(std::string* body, const std::string& value)
{
	size_t eq = body->find('=');
	size_t start = body->find_first_not_of(" \t", eq + 1);
	size_t end = body->find_last_not_of(" \t\r\n") + 1;
	*body = body->substr(0, start) + value + body->substr(end);
}

std::string RewriteIniKeys(const std::string& text, const std::vector<IniEntry>& entries,
                           const char* appendSection)
{
	std::vector<IniLine> lines;
	SplitLines(text, &lines);

	std::string eol = "\r\n";
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (!lines[i].eol.empty())
		{
			eol = lines[i].eol;
			break;
		}
	}

	std::vector<bool> seen(entries.size(), false);
	int sectionEnd = -1;    // last non-blank line of appendSection
	int firstHeader = -1;
	int preambleEnd = -1;   // last non-blank line before the first section header
	bool inSection = false;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string trimmed = IniTrim(lines[i].body);
		if (!trimmed.empty() && trimmed[0] == '[')
		{
			if (firstHeader < 0)
				firstHeader = (int)i;
			inSection = appendSection && SameHeader(trimmed, appendSection);
			if (inSection)
				sectionEnd = (int)i;
			continue;
		}
		if (!trimmed.empty())
		{
			if (inSection)
				sectionEnd = (int)i;
			if (firstHeader < 0)
				preambleEnd = (int)i;
		}

		std::string key, val;
		if (!SplitIniLine(lines[i].body, &key, &val))
			continue;
		for (size_t e = 0; e < entries.size(); ++e)
		{
			if (entries[e].key != key)
				continue;
			seen[e] = true;
			if (!IniValueEquals(entries[e].kind, val, entries[e].value))
				ReplaceValue(&lines[i].body, entries[e].value);
			break;
		}
	}

	std::vector<IniLine> added;
	for (size_t e = 0; e < entries.size(); ++e)
	{
		if (seen[e] || !entries[e].append)
			continue;
		IniLine l;
		l.body = entries[e].key + "=" + entries[e].value;
		l.eol = eol;
		added.push_back(l);
	}
	if (!added.empty())
	{
		size_t at = lines.size();
		if (appendSection && sectionEnd >= 0)
			at = (size_t)sectionEnd + 1;
		else if (!appendSection && firstHeader >= 0)
			at = (size_t)(preambleEnd + 1);
		if (at > 0 && lines[at - 1].eol.empty())
			lines[at - 1].eol = eol;
		lines.insert(lines.begin() + at, added.begin(), added.end());
	}

	std::string out;
	out.reserve(text.size() + added.size() * 40);
	for (size_t i = 0; i < lines.size(); ++i)
	{
		out += lines[i].body;
		out += lines[i].eol;
	}
	return out;
}
