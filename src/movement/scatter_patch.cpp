// scatter_patch.cpp - One-time scatter branch patch at startup.
// Main thread; the guarded pointer read keeps its own POD-only SEH scope.

#include "movement/formation.h"
#include "pathfind/order_hook.h"

// Binary-patch scatter branch in addOrderSelectedCharacters
//
// The function at RVA_ADD_ORDER_SELECTED has a branch:
//   if (i == *v31)    // is this the leader?
//       vtable+792(char, building, subject, exactDest);
//   else
//       scatter calculation using sqrtf...
//       vtable+792(char, building, subject, scatteredDest);
//
// We patch the conditional jump to force ALL characters through the leader
// code path, giving everyone the exact click destination (zero scatter).
// Arrival scatter is handled separately by PollFormationGroups.

// The file's guarded-read pattern
// (base/prologue.cpp's ReadGameBytes16 -- __try/__except with GuardEnter/GuardLeave,
// core.h, so the fault never reaches the crash recorder) for the FF 25
// pointer-slot dereference below. Standalone and POD-only on purpose: MSVC
// 2010 rejects __try in a function that also holds objects needing
// unwinding, and ApplyScatterPatch uses std::ostringstream.
static bool ReadPointerGuarded(const void* addr, uintptr_t* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		*out = *(const uintptr_t*)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

bool ApplyScatterPatch()
{
	// VerifyPrologue still runs below and is still fatal on a genuine
	// mismatch or a genuine foreign hook -- nothing here weakens that check.
	//
	// "addOrderSelected" is also an UNCONDITIONAL row in g_hookPrologues
	// (plugin/hook_manifest_rows.inc): plugin_entry.cpp's build-gate loop verifies it against the game's
	// own, unhooked prologue before any hook installs, and a mismatch there
	// refuses every install (this one included) before startPlugin gets this
	// far. By the time ApplyScatterPatch runs, our own addOrderSelected hook
	// is already installed at this RVA -- hook_manifest.cpp only calls us when
	// orig_addOrderSelected is non-NULL -- so the first bytes here are USUALLY
	// our own detour, not the game's. Re-running VerifyPrologueByRva on our
	// own detour finds it, and (correctly, by its own contract) reports it as
	// "already hooked by another plugin" -- which is misleading when the
	// detour is ours. Distinguish the two: only skip the redundant re-check
	// when the immediate jump target is our own hook_addOrderSelected; a
	// foreign detour (another plugin hooked first, ours chained after) or any
	// unrecognised prologue still goes through the full check.
	//
	// Both detour shapes VerifyPrologue and DetourLength recognise:
	// 5-byte `E9 rel32` (near jump) and 6-byte `FF 25 rel32` (RIP-relative
	// indirect jump through an 8-byte pointer -- MinHook uses this one when
	// the target is out of E9's +/-2GB range). DetourLength is inline in
	// game/prologue_policy.h and classifies detour length for a tail-match.
	// VerifyPrologue in base/prologue.cpp uses that policy; neither helper
	// resolves the target needed by this own-hook check. The local decode
	// below preserves the two jump shapes and uses ReadPointerGuarded for
	// the indirect pointer slot. Its GuardEnter/GuardLeave scope stays
	// separate from ApplyScatterPatch's diagnostic stream objects.
	bool ownDetour = false;
	{
		unsigned char* site = (unsigned char*)((uintptr_t)GameAddr(RVA_ADD_ORDER_SELECTED));
		uintptr_t target = 0;
		bool haveTarget = false;
		if (site[0] == 0xE9)
		{
			int rel32 = *(int*)(site + 1);
			target = (uintptr_t)(site + 5) + (uintptr_t)rel32;
			haveTarget = true;
		}
		else if (site[0] == 0xFF && site[1] == 0x25)
		{
			int rel32 = *(int*)(site + 2);
			uintptr_t ptrAddr = (uintptr_t)(site + 6) + (uintptr_t)rel32;
			// FF 25 rel32 = jmp qword ptr [rip+rel32]: the target is the
			// 8-byte value stored AT ptrAddr, not ptrAddr itself. Guarded: on
			// an unexpected binary layout ptrAddr is an arbitrary computed
			// address, not something VerifyPrologueByRva has vetted yet.
			uintptr_t ptrValue = 0;
			if (ReadPointerGuarded((const void*)ptrAddr, &ptrValue))
			{
				target = ptrValue;
				haveTarget = true;
			}
		}
		if (haveTarget)
			ownDetour = (target == (uintptr_t)&hook_addOrderSelected);
	}

	if (ownDetour)
	{
		LogMsg("Scatter patch: addOrderSelected already carries our own "
		       "hook (prologue verified by the build gate at startup); skipping "
		       "the redundant re-check");
	}
	else if (!VerifyPrologueByRva(RVA_ADD_ORDER_SELECTED))
	{
		return false;
	}

	uintptr_t funcBase = (uintptr_t)GameAddr(RVA_ADD_ORDER_SELECTED);
	const int funcSize = 1714;
	unsigned char* funcBytes = (unsigned char*)funcBase;
	uintptr_t sqrtfAddr = gameBase + RVA_SQRTF;

	// Step 1: Find the 'call sqrtf' instruction (E8 rel32).
	// This call appears once in the function, inside the scatter block.
	int callOffset = -1;
	for (int i = 0; i < funcSize - 5; ++i)
	{
		if (funcBytes[i] == 0xE8)
		{
			int rel32 = *(int*)(funcBytes + i + 1);
			uintptr_t target = (uintptr_t)(funcBytes + i + 5) + rel32;
			if (target == sqrtfAddr)
			{
				callOffset = i;
				break;
			}
		}
	}

	if (callOffset < 0)
	{
		LogMsg("Scatter patch: sqrtf call not found");
		return false;
	}

	// Step 2: Walk backward from sqrtf call to find the preceding
	// conditional jump (the leader check). Look for JE/JNE in both
	// short (2-byte) and near (6-byte) forms.
	int patchOffset = -1;
	int jumpLen = 0;
	bool isJE = false;  // true = JE (leader is jump target), false = JNE (scatter is jump target)

	for (int scan = callOffset - 2; scan >= 0 && scan >= callOffset - 128; --scan)
	{
		// 6-byte near conditional: 0F 84 (JE) or 0F 85 (JNE)
		if (scan >= 1 && funcBytes[scan - 1] == 0x0F)
		{
			if (funcBytes[scan] == 0x84)
			{
				patchOffset = scan - 1;
				jumpLen = 6;
				isJE = true;
				break;
			}
			if (funcBytes[scan] == 0x85)
			{
				patchOffset = scan - 1;
				jumpLen = 6;
				isJE = false;
				break;
			}
		}
		// 2-byte short conditional: 74 (JE) or 75 (JNE)
		if (funcBytes[scan] == 0x74)
		{
			patchOffset = scan;
			jumpLen = 2;
			isJE = true;
			break;
		}
		if (funcBytes[scan] == 0x75)
		{
			patchOffset = scan;
			jumpLen = 2;
			isJE = false;
			break;
		}
	}

	if (patchOffset < 0)
	{
		LogMsg("Scatter patch: conditional jump not found before sqrtf");
		return false;
	}

	// Step 3: Patch the conditional jump.
	unsigned char* patchAddr = funcBytes + patchOffset;

	DWORD oldProtect;
	if (!VirtualProtect(patchAddr, jumpLen, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		LogMsg("Scatter patch: VirtualProtect failed");
		return false;
	}

	if (isJE)
	{
		// JE: leader path is the jump target. Make unconditional.
		if (jumpLen == 6)
		{
			// 0F 84 rel32 -> 90 E9 rel32 (NOP + JMP near, same target)
			int rel32 = *(int*)(patchAddr + 2);
			patchAddr[0] = 0x90;  // NOP (absorbs the extra byte)
			patchAddr[1] = 0xE9;  // JMP near
			*(int*)(patchAddr + 2) = rel32;  // rel32 is relative to end of JE (6 bytes) but
			                                  // JMP end is at patchAddr+6 too, so same offset
		}
		else
		{
			// 74 rel8 -> EB rel8 (JMP short)
			patchAddr[0] = 0xEB;
		}
	}
	else
	{
		// JNE: scatter path is the jump target. NOP it so all chars
		// fall through to the leader path.
		for (int b = 0; b < jumpLen; ++b)
			patchAddr[b] = 0x90;
	}

	VirtualProtect(patchAddr, jumpLen, oldProtect, &oldProtect);

	{
		std::ostringstream ss;
		ss << "Scatter patch: " << (isJE ? "JE" : "JNE")
		   << " at func+" << patchOffset << " (" << jumpLen << " bytes)"
		   << ", sqrtf at func+" << callOffset;
		LogMsg(ss.str());
	}

	scatterPatchApplied = true;
	return true;
}
