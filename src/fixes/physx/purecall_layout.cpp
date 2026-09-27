#include "fixes/physx/purecall_layout.h"

const unsigned __int64 kPurecallRva = 0x47624;
const unsigned __int64 kPurecallHandlerPtrRva = 0x40AF80;

const int kPurecallSignatureLen = 11;
const unsigned char kPurecallSignature[11] =
{
	0x48, 0x83, 0xEC, 0x28,                   // sub rsp, 28h
	0x48, 0x8B, 0x0D, 0x51, 0x39, 0x3C, 0x00   // mov rcx, cs:qword_18040AF80
};

bool PurecallSignatureMatches(const unsigned char* bytes, int len)
{
	if (!bytes || len < kPurecallSignatureLen)
		return false;
	for (int i = 0; i < kPurecallSignatureLen; ++i)
	{
		if (bytes[i] != kPurecallSignature[i])
			return false;
	}
	return true;
}

const unsigned __int64 kCulpritSlotOffset = 0x30;

unsigned __int64 CulpritSlotAddress(unsigned __int64 handlerReturnSlotAddr)
{
	return handlerReturnSlotAddr + kCulpritSlotOffset;
}

bool AddressInRange(unsigned __int64 addr, unsigned __int64 base, unsigned __int64 size)
{
	if (base == 0 || size == 0)
		return false;
	return addr >= base && (addr - base) < size;
}

PurecallModuleClass ClassifyPurecallCulprit(unsigned __int64 addr,
	unsigned __int64 gameBase, unsigned __int64 gameSize,
	unsigned __int64 physxBase, unsigned __int64 physxSize)
{
	if (addr == 0)
		return PURECALL_MOD_UNKNOWN;
	if (AddressInRange(addr, gameBase, gameSize))
		return PURECALL_MOD_EXE;
	if (AddressInRange(addr, physxBase, physxSize))
		return PURECALL_MOD_PHYSX;
	return PURECALL_MOD_UNKNOWN;
}

// Same cadence as core.cpp's ProfilerImageResolveTick: once a second, give up
// after a minute. Both wait on a DLL the exe loads for itself at some point
// during startup, not on anything this mod controls.
const double kPurecallRetryIntervalSec = 1.0;
const double kPurecallRetryWindowSec = 60.0;

bool PurecallRetryDue(double now, double lastAttempt)
{
	if (lastAttempt < 0.0)
		return true;
	return (now - lastAttempt) >= kPurecallRetryIntervalSec;
}

bool PurecallRetryExpired(double now, double firstAttempt)
{
	if (firstAttempt < 0.0)
		return false;
	return (now - firstAttempt) >= kPurecallRetryWindowSec;
}
