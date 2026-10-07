// dialogue_thunk_policy.cpp - The dialogue call site's bytes, the thunk builder and the call's
// rel32. Pure; any thread.
#include "inventory/dialogue_thunk_policy.h"
#include <string.h>

namespace keo_inventory {

// Read from the IDB (kenshi_x64.exe, Steam 1.0.65) 2026-09-30, DialogLineData::checkConditions.
const unsigned __int64 kDialogLeadRva = 0x678998;
const unsigned __int64 kDialogCallRva = 0x6789AA;
const unsigned char kDialogLeadBytes[23] =
{
	0x49, 0x8B, 0x04, 0x24,                     // mov  rax, [r12]
	0x49, 0x8B, 0xCC,                           // mov  rcx, r12
	0xFF, 0x90, 0x60, 0x01, 0x00, 0x00,         // call [rax+160h]
	0x8B, 0xD3,                                 // mov  edx, ebx
	0x48, 0x8B, 0xC8,                           // mov  rcx, rax
	0xE8, 0x63, 0x2E, 0x9D, 0xFF                // call hasItemFunction
};

bool DialogLeadMatches(const unsigned char* actual)
{
	if (!actual)
		return false;
	return memcmp(actual, kDialogLeadBytes, (size_t)kDialogLeadLen) == 0;
}

bool DialogThunkBuild(unsigned char* out, size_t cap, unsigned __int64 wrapperAddr)
{
	if (!out || cap < kDialogThunkLen || wrapperAddr == 0)
		return false;
	static const unsigned char kHead[9] =
	{
		0x4D, 0x8B, 0xC4,                       // mov  r8, r12
		0xFF, 0x25, 0x00, 0x00, 0x00, 0x00      // jmp  qword ptr [rip+0]
	};
	memcpy(out, kHead, sizeof(kHead));
	for (int i = 0; i < 8; ++i)
		out[9 + i] = (unsigned char)(wrapperAddr >> (8 * i));
	return true;
}

bool DialogCallRel32(unsigned __int64 callAddr, unsigned __int64 thunkAddr, int* rel32Out)
{
	const __int64 delta = (__int64)thunkAddr - (__int64)(callAddr + 5);
	if (!(delta >= (__int64)-0x80000000LL && delta <= (__int64)0x7FFFFFFFLL))
		return false;
	if (rel32Out)
		*rel32Out = (int)delta;
	return true;
}

} // namespace keo_inventory
