#pragma once
#include <stddef.h>

inline char NameListLower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// True when name contains any comma-separated token of list, compared
// case-insensitively. Spaces around a token are ignored and empty tokens
// match nothing.
inline bool NameMatchesList(const char* name, const char* list)
{
	if (!name || !list || !name[0])
		return false;
	const char* tok = list;
	while (*tok)
	{
		while (*tok == ' ' || *tok == ',')
			++tok;
		const char* end = tok;
		while (*end && *end != ',')
			++end;
		const char* last = end;
		while (last > tok && last[-1] == ' ')
			--last;
		size_t len = (size_t)(last - tok);
		if (len)
		{
			for (const char* s = name; *s; ++s)
			{
				size_t i = 0;
				while (i < len && s[i] && NameListLower(s[i]) == NameListLower(tok[i]))
					++i;
				if (i == len)
					return true;
			}
		}
		tok = end;
	}
	return false;
}
