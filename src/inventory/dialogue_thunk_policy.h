// dialogue_thunk_policy.h - The dialogue item-function call site and the near-page thunk that
// hands the tested character to the wrapper. Pure: no Windows, KenshiLib or game header.
#ifndef KEO_INVENTORY_DIALOGUE_THUNK_POLICY_H
#define KEO_INVENTORY_DIALOGUE_THUNK_POLICY_H

#include <stddef.h>

namespace keo_inventory {

// checkConditions' one hasItemFunction call, and the instructions that pin r12 as the tested
// character in front of it:
//   678998  49 8B 04 24        mov  rax, [r12]
//   67899C  49 8B CC           mov  rcx, r12
//   67899F  FF 90 60 01 00 00  call [rax+160h]      ; getInventory
//   6789A5  8B D3              mov  edx, ebx        ; ItemFunction
//   6789A7  48 8B C8           mov  rcx, rax
//   6789AA  E8 63 2E 9D FF     call hasItemFunction ; the rel32 at 6789AB is rewritten
extern const unsigned __int64 kDialogLeadRva;     // 0x678998
extern const unsigned __int64 kDialogCallRva;     // 0x6789AA
const int kDialogLeadLen = 23;
extern const unsigned char kDialogLeadBytes[23];

// Exact compare: mid-function, so a difference means an unknown binary.
bool DialogLeadMatches(const unsigned char* actual);

// The thunk: mov r8, r12; jmp qword ptr [rip+0]; <wrapper, 8 bytes>.
const size_t kDialogThunkLen = 17;
bool DialogThunkBuild(unsigned char* out, size_t cap, unsigned __int64 wrapperAddr);

// The call's new rel32 to the thunk; false when the thunk is out of rel32 reach of the call's next
// instruction.
bool DialogCallRel32(unsigned __int64 callAddr, unsigned __int64 thunkAddr, int* rel32Out);

} // namespace keo_inventory

#endif
