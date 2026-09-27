#pragma once
#include <string.h>

// Byte-only part of the production build gate; the caller owns guarded reads.
enum PrologueClass
{
	PROLOGUE_UNREADABLE,
	PROLOGUE_MISMATCH,
	PROLOGUE_ORIGINAL,
	PROLOGUE_SHARED
};

// A site another plugin has already detoured starts with the detour, not with
// the function's own first instruction. MinHook-style patches are either a
// 5-byte `E9 rel32` or a 6-byte `FF 25 rel32` (jmp [rip+disp32]), and MinHook
// may pad with 0x90/0xCC out to the next instruction boundary. The bytes past
// the detour still belong to the real function, so they are what we verify.
// Returns the detour length (5 or 6), or 0 if this is not a detour.
inline int DetourLength(const unsigned char* b)
{
	if (b[0] == 0xE9)
		return 5;
	if (b[0] == 0xFF && b[1] == 0x25)
		return 6;
	return 0;
}

// True if the bytes past a detour still match the expected prologue, either
// directly from the detour's end or after 0x90/0xCC padding up to offset 8.
inline bool PrologueTailMatches(const unsigned char* actual,
                                const unsigned char* expect, int start)
{
	if (memcmp(actual + start, expect + start, 16 - start) == 0)
		return true;

	// Padding variant: MinHook copied a longer instruction and padded the
	// remainder of it. Only the bytes between the detour and offset 8 may be
	// padding; everything from 8 on must match.
	for (int i = start; i < 8; ++i)
	{
		if (actual[i] != 0x90 && actual[i] != 0xCC)
			return false;
	}
	return memcmp(actual + 8, expect + 8, 8) == 0;
}

inline PrologueClass ClassifyPrologue(const unsigned char* actual,
                                      const unsigned char* expect, bool readable)
{
	if (!readable || !actual || !expect) return PROLOGUE_UNREADABLE;
	if (memcmp(actual, expect, 16) == 0) return PROLOGUE_ORIGINAL;
	int detour = DetourLength(actual);
	return detour > 0 && PrologueTailMatches(actual, expect, detour)
		? PROLOGUE_SHARED : PROLOGUE_MISMATCH;
}
