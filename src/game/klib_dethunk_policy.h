#pragma once
#include <stddef.h>
#include <stdint.h>

// Follows an import thunk in our own image to the function it jumps to.
//
// When the compiler takes the address of an imported function without going
// through its import slot, the linker satisfies the reference with a local
// thunk, FF 25 rel32 (jmp qword ptr [rip+rel32]), whose slot is in our own
// import address table. KenshiLib::GetRealAddress refuses any address inside
// the caller's image, so such a thunk has to be followed first. An address
// outside our image is returned unchanged, and so is anything inside it that
// is not an import thunk.
//
// image is our image as mapped: image[0] is the byte at base, and imageSize
// bytes follow. iatOffset and iatSize place our import address table (the
// IAT data directory) inside it; the slot must be a whole qword of that
// table, so a jmp [rip+X] through any other pointer in the image, such as a
// function-pointer global, is not followed. Nothing outside
// [image, image + imageSize) is read, so the caller needs no fault guard. No
// Windows header, so a host test can hand in a fabricated image.

enum KlibDethunkVerdict
{
	KLIB_DETHUNK_OUTSIDE = 0,   // not in our image: used as-is
	KLIB_DETHUNK_FOLLOWED,      // our import thunk: its slot's value is used
	KLIB_DETHUNK_NOT_THUNK      // in our image but not an import thunk: used as-is
};

const size_t KLIB_THUNK_BYTES = 6;   // FF 25 and a 32-bit displacement
const size_t KLIB_SLOT_BYTES  = 8;

// *resolved receives the address to use: the slot's value on FOLLOWED, the
// input address otherwise.
inline KlibDethunkVerdict KlibDethunk(uintptr_t address, uintptr_t base,
                                      const unsigned char* image, size_t imageSize,
                                      size_t iatOffset, size_t iatSize,
                                      uintptr_t* resolved)
{
	*resolved = address;
	if (address < base || address - base >= imageSize)
		return KLIB_DETHUNK_OUTSIDE;

	size_t at = (size_t)(address - base);
	if (imageSize - at < KLIB_THUNK_BYTES)
		return KLIB_DETHUNK_NOT_THUNK;
	const unsigned char* op = image + at;
	if (op[0] != 0xFF || op[1] != 0x25)
		return KLIB_DETHUNK_NOT_THUNK;

	// The displacement is signed and counts from the end of the instruction.
	uint32_t raw = (uint32_t)op[2] | ((uint32_t)op[3] << 8)
	             | ((uint32_t)op[4] << 16) | ((uint32_t)op[5] << 24);
	uintptr_t slot = address + KLIB_THUNK_BYTES + (uintptr_t)(intptr_t)(int32_t)raw;

	// An import thunk's slot is in our own import address table. A jump
	// through anything else is not one of ours. The table itself must lie
	// inside the image, so the read below stays inside it too.
	if (iatOffset > imageSize || iatSize > imageSize - iatOffset || iatSize < KLIB_SLOT_BYTES)
		return KLIB_DETHUNK_NOT_THUNK;
	if (slot < base + iatOffset || slot - (base + iatOffset) > iatSize - KLIB_SLOT_BYTES)
		return KLIB_DETHUNK_NOT_THUNK;
	const unsigned char* s = image + (size_t)(slot - base);
	uint64_t target = 0;
	for (size_t i = 0; i < KLIB_SLOT_BYTES; ++i)
		target |= (uint64_t)s[i] << (8 * i);
	// An empty slot is not a bound import; the thunk address is kept, so the
	// caller's own check reports it.
	if (!target)
		return KLIB_DETHUNK_NOT_THUNK;

	*resolved = (uintptr_t)target;
	return KLIB_DETHUNK_FOLLOWED;
}
