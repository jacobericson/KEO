#include <cstdio>
#include <cstring>
#include "fixes/physx/purecall_layout.h"

#include "check.h"

int main()
{
	// The exact signature verified in IDA against PhysXCore64.dll (Steam
	// 1.0.65): sub rsp,28h; mov rcx,cs:qword_18040AF80.
	unsigned char good[16] =
	{
		0x48, 0x83, 0xEC, 0x28,
		0x48, 0x8B, 0x0D, 0x51, 0x39, 0x3C, 0x00,
		0xE8, 0x28, 0x20, 0x00, 0x00 // call DecodePointer -- past the checked prefix
	};
	Check(PurecallSignatureMatches(good, 16), "the real bytes match");
	Check(PurecallSignatureMatches(good, kPurecallSignatureLen), "an exact-length buffer still matches");
	Check(!PurecallSignatureMatches(good, kPurecallSignatureLen - 1), "a short buffer is rejected");
	Check(!PurecallSignatureMatches(NULL, 16), "a null buffer is rejected");

	{
		unsigned char bad[16];
		memcpy(bad, good, sizeof(bad));
		bad[3] = 0x30; // sub rsp, 30h instead of 28h -- a different build
		Check(!PurecallSignatureMatches(bad, 16), "a changed opcode byte is rejected");
	}
	{
		unsigned char bad[16];
		memcpy(bad, good, sizeof(bad));
		bad[7] = 0x00; // the mov's displacement resolves somewhere else now
		Check(!PurecallSignatureMatches(bad, 16), "a changed displacement byte is rejected");
	}

	// _purecall calls the handler with `call rax`: the handler's own return
	// slot holds an address inside _purecall, and the real culprit -- the
	// call site that dereferenced the freed shape -- sits 0x30 bytes above
	// that slot (8 bytes for the `call`'s own return address, plus the 0x28
	// _purecall subtracted from rsp at its own entry).
	Check(CulpritSlotAddress(0x1000) == 0x1030, "culprit slot is 0x30 above the handler's own return slot");
	Check(CulpritSlotAddress(0) == 0x30, "the offset is added even from a zero base (arithmetic only)");

	Check(AddressInRange(0x1000, 0x1000, 0x100), "range start is inclusive");
	Check(AddressInRange(0x10FF, 0x1000, 0x100), "last byte in range is inside");
	Check(!AddressInRange(0x1100, 0x1000, 0x100), "one past the end is outside");
	Check(!AddressInRange(0x0FFF, 0x1000, 0x100), "one before the start is outside");
	Check(!AddressInRange(0x1050, 0, 0x100), "a zero base never matches (unresolved module)");
	Check(!AddressInRange(0x1050, 0x1000, 0), "a zero size never matches (unresolved module)");

	Check(ClassifyPurecallCulprit(0, 0x1000, 0x100, 0x2000, 0x100) == PURECALL_MOD_UNKNOWN,
	      "a null culprit is unknown");
	Check(ClassifyPurecallCulprit(0x1050, 0x1000, 0x100, 0x2000, 0x100) == PURECALL_MOD_EXE,
	      "an address inside the exe range classifies as exe");
	Check(ClassifyPurecallCulprit(0x2050, 0x1000, 0x100, 0x2000, 0x100) == PURECALL_MOD_PHYSX,
	      "an address inside the physx range classifies as physx");
	Check(ClassifyPurecallCulprit(0x3050, 0x1000, 0x100, 0x2000, 0x100) == PURECALL_MOD_UNKNOWN,
	      "an address in neither range is unknown");
	Check(ClassifyPurecallCulprit(0x2050, 0, 0, 0, 0) == PURECALL_MOD_UNKNOWN,
	      "unresolved ranges never falsely classify");

	// Deferred-install retry policy: PhysXCore64.dll usually isn't loaded at
	// plugin init, so the install is retried on a throttled, bounded cadence.
	Check(PurecallRetryDue(0.0, -1.0), "no attempt yet is always due");
	Check(!PurecallRetryDue(1.0, 0.5), "half the interval since the last try is not due");
	Check(PurecallRetryDue(1.5, 0.5), "a full interval since the last try is due");
	Check(PurecallRetryDue(10.0, 0.5), "well past the interval is still due");

	Check(!PurecallRetryExpired(30.0, 0.0), "well inside the window has not expired");
	Check(!PurecallRetryExpired(59.9, 0.0), "just under the window has not expired");
	Check(PurecallRetryExpired(60.0, 0.0), "exactly the window boundary has expired");
	Check(PurecallRetryExpired(120.0, 0.0), "well past the window has expired");
	Check(!PurecallRetryExpired(5.0, -1.0), "no first attempt yet never reads as expired");

	return CheckExit("purecall_layout_units");
}
