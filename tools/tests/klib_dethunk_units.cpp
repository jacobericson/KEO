#include <cstdio>
#include <cstring>
#include "game/klib_dethunk_policy.h"

#include "check.h"

// A fabricated image: IMAGE_SIZE bytes that the policy is told are mapped at
// BASE, which is deliberately not the buffer's own address, so every read
// has to go through the base-relative offset.
static const uintptr_t BASE       = (uintptr_t)0x180000000ULL;
static const size_t    IMAGE_SIZE = 0x100;
static unsigned char   g_image[IMAGE_SIZE];

static void Clear() { memset(g_image, 0xCC, sizeof(g_image)); }

// FF 25 rel32 at offset `at`, jumping through the slot at offset `slot`.
static void PutThunk(size_t at, size_t slot)
{
	int32_t rel = (int32_t)((intptr_t)slot - (intptr_t)(at + KLIB_THUNK_BYTES));
	g_image[at] = 0xFF;
	g_image[at + 1] = 0x25;
	for (int i = 0; i < 4; ++i)
		g_image[at + 2 + i] = (unsigned char)((uint32_t)rel >> (8 * i));
}

static void PutSlot(size_t slot, uint64_t value)
{
	for (int i = 0; i < 8; ++i)
		g_image[slot + i] = (unsigned char)(value >> (8 * i));
}

// The import address table the policy is told about. The cases before the
// IAT section let it span the whole image, so they exercise the image bounds.
static size_t g_iatOffset = 0;
static size_t g_iatSize   = IMAGE_SIZE;

static KlibDethunkVerdict Run(uintptr_t address, uintptr_t* resolved)
{
	return KlibDethunk(address, BASE, g_image, IMAGE_SIZE, g_iatOffset, g_iatSize, resolved);
}

int main()
{
	const uint64_t KLIB_STUB  = 0x00007FFA12345678ULL;   // an address in KenshiLib.dll
	const uint64_t KLIB_STUB2 = 0x00007FFA0BADF00DULL;
	uintptr_t out = 0;

	// Outside our image: passed through, the image never consulted.
	{
		Clear();
		Check(Run(BASE - 1, &out) == KLIB_DETHUNK_OUTSIDE && out == BASE - 1, "one byte below base is outside");
		Check(Run(BASE + IMAGE_SIZE, &out) == KLIB_DETHUNK_OUTSIDE && out == BASE + IMAGE_SIZE,
		      "end of image is outside");
		Check(Run((uintptr_t)KLIB_STUB, &out) == KLIB_DETHUNK_OUTSIDE && out == (uintptr_t)KLIB_STUB,
		      "a KenshiLib address is outside and unchanged");
		Check(Run(0, &out) == KLIB_DETHUNK_OUTSIDE && out == 0, "null is outside");
		// A thunk pattern that would decode is irrelevant when the address is
		// not ours: the policy must not read through someone else's jump.
		unsigned char* noImage = NULL;
		Check(KlibDethunk((uintptr_t)KLIB_STUB, BASE, noImage, IMAGE_SIZE, 0, IMAGE_SIZE, &out) == KLIB_DETHUNK_OUTSIDE,
		      "outside is decided without reading the image");
	}

	// A well-formed thunk with a positive displacement (slot after the thunk).
	{
		Clear();
		PutThunk(0x10, 0x80);
		PutSlot(0x80, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB,
		      "positive rel32 thunk follows its slot");
		Check(g_image[0x12] == 0x6A && g_image[0x13] == 0 && g_image[0x15] == 0, "positive rel32 encoded as 0x6A");
	}

	// A well-formed thunk with a negative displacement (slot before the thunk),
	// the layout a linker produces when the import table precedes the code.
	{
		Clear();
		PutThunk(0xC0, 0x20);
		PutSlot(0x20, KLIB_STUB2);
		Check(Run(BASE + 0xC0, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB2,
		      "negative rel32 thunk follows its slot");
		Check(g_image[0xC5] == 0xFF, "negative rel32 has its sign byte set");
	}

	// Inside our image but not an import thunk: passed through unchanged.
	{
		Clear();
		const unsigned char prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 };   // mov [rsp+8], rbx; push rdi
		memcpy(g_image + 0x30, prologue, sizeof(prologue));
		Check(Run(BASE + 0x30, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x30, "a function prologue is not a thunk");

		PutThunk(0x40, 0x80);
		PutSlot(0x80, KLIB_STUB);
		g_image[0x41] = 0x15;   // FF 15: call qword ptr [rip+rel32]
		Check(Run(BASE + 0x40, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x40, "FF 15 (call) is not a thunk");

		g_image[0x50] = 0xE9;   // jmp rel32 (an incremental-link or hook jump)
		Check(Run(BASE + 0x50, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x50, "E9 jmp is not an import thunk");

		Check(Run(BASE + 0x41, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x41,
		      "an address one byte into a thunk is not a thunk");
	}

	// The slot must be a whole qword inside our image.
	{
		Clear();
		PutThunk(0x10, IMAGE_SIZE + 0x10);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "slot past the end of the image: not ours");

		Clear();
		PutThunk(0x10, IMAGE_SIZE - 7);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "slot straddling the end of the image: not ours");

		Clear();
		PutThunk(0x10, IMAGE_SIZE - 8);
		PutSlot(IMAGE_SIZE - 8, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB,
		      "slot in the last qword of the image is followed");

		Clear();
		g_image[0x10] = 0xFF;
		g_image[0x11] = 0x25;
		g_image[0x12] = 0x00; g_image[0x13] = 0x00; g_image[0x14] = 0x00; g_image[0x15] = 0x80;   // rel32 = INT32_MIN
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "slot 2 GB below the image: not ours");
	}

	// An unbound (zero) slot keeps the thunk address.
	{
		Clear();
		PutThunk(0x10, 0x80);
		PutSlot(0x80, 0);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10, "an empty slot is not followed");
	}

	// The boundary: a thunk whose six bytes end exactly at the end of the image.
	{
		Clear();
		PutThunk(IMAGE_SIZE - 6, 0x20);
		PutSlot(0x20, KLIB_STUB);
		Check(Run(BASE + IMAGE_SIZE - 6, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB,
		      "thunk at end - 6 is read in full and followed");

		Clear();
		g_image[IMAGE_SIZE - 5] = 0xFF;
		g_image[IMAGE_SIZE - 4] = 0x25;
		Check(Run(BASE + IMAGE_SIZE - 5, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + IMAGE_SIZE - 5,
		      "FF 25 at end - 5 cannot hold a displacement: not a thunk");
		Check(Run(BASE + IMAGE_SIZE - 1, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + IMAGE_SIZE - 1,
		      "last byte of the image is inside but not a thunk");
	}

	// An image too small to hold a slot at all.
	{
		unsigned char tiny[6] = { 0xFF, 0x25, 0xFA, 0xFF, 0xFF, 0xFF };   // slot = the thunk itself
		Check(KlibDethunk(BASE, BASE, tiny, sizeof(tiny), 0, sizeof(tiny), &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE,
		      "a six-byte image holds no slot");
	}

	// The slot must be a whole qword of the import address table, not merely
	// of the image: a jmp [rip+X] through a function-pointer global elsewhere
	// in the image is not an import thunk.
	{
		const size_t IAT = 0x80, IAT_SIZE = 0x20;   // slots 0x80, 0x88, 0x90, 0x98
		g_iatOffset = IAT;
		g_iatSize = IAT_SIZE;

		Clear();
		PutThunk(0x10, IAT);
		PutSlot(IAT, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB,
		      "slot at the first qword of the IAT is followed");

		Clear();
		PutThunk(0x10, IAT + IAT_SIZE - 8);
		PutSlot(IAT + IAT_SIZE - 8, KLIB_STUB2);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_FOLLOWED && out == (uintptr_t)KLIB_STUB2,
		      "slot at the last qword of the IAT is followed");

		Clear();
		PutThunk(0x10, IAT + IAT_SIZE - 7);
		PutSlot(IAT + IAT_SIZE - 7, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "slot straddling the end of the IAT is not followed");

		Clear();
		PutThunk(0x10, IAT - 8);
		PutSlot(IAT - 8, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "slot just below the IAT is not followed");

		Clear();
		PutThunk(0xC0, 0x20);   // a function-pointer global at 0x20, inside the image
		PutSlot(0x20, KLIB_STUB);
		Check(Run(BASE + 0xC0, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0xC0,
		      "a jmp through a pointer outside the IAT is not followed");

		g_iatSize = 0;
		Clear();
		PutThunk(0x10, IAT);
		PutSlot(IAT, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "no IAT: nothing is followed");

		g_iatOffset = IMAGE_SIZE - 0x10;
		g_iatSize = 0x20;   // runs past the end of the image
		Clear();
		PutThunk(0x10, IMAGE_SIZE - 0x10);
		PutSlot(IMAGE_SIZE - 0x10, KLIB_STUB);
		Check(Run(BASE + 0x10, &out) == KLIB_DETHUNK_NOT_THUNK && out == BASE + 0x10,
		      "an IAT reaching past the image is not trusted");

		g_iatOffset = 0;
		g_iatSize = IMAGE_SIZE;
	}

	return CheckExit("klib_dethunk_units");
}
