#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "diag/cpp_exception.h"
#include "diag/fatal_class.h"

// The per-thread guard counter that keeps the mod's own deliberate faults out
// of the crash recorder. Declared here rather than pulled in with core.h, so
// this file compiles with no game headers; core.cpp owns the definition.
extern __declspec(thread) int g_inOurGuard;

namespace cpp_exception_detail {

const size_t TYPE_CAP = 128;

// Any-thread CppExceptionNote updates count/address atomically and fills
// the last-type bytes before publishing their length and ThrowInfo. Main
// token reporters and any-thread fault readers copy those live bytes without
// a sequence check; mixed diagnostic names or independently updated fields
// are tolerated. Session state is never reset; recordSeq is a separate claim.
static volatile LONG   g_count = 0;
static volatile LONG   g_recordSeq = 0;
static volatile LONG   g_typeLen = 0;
static volatile LONG64 g_lastAddr = 0;
static volatile LONG64 g_lastThrowInfo = 0;
static double          g_lastWriteSec = -1.0;
static char            g_typeName[TYPE_CAP] = { 0 };

// The throw's four parameters. The magic distinguishes a real throw from any
// other use of the code, and a rethrow carries none of them.
static bool IsThrowWithTypeInfo(const EXCEPTION_RECORD* er)
{
	if (er->NumberParameters < 4)
		return false;
	ULONG_PTR magic = er->ExceptionInformation[0];
	return magic == 0x19930520 || magic == 0x19930521 || magic == 0x19930522;
}

// Standalone and POD-only: MSVC 2010 rejects __try in a function holding
// objects that need unwinding. A fault here re-enters the vectored handler,
// which declines while the guard is held, and this __except takes it.
static bool ReadBytesGuarded(const void* addr, void* out, size_t size)
{
	bool ok = true;
	++g_inOurGuard;
	__try
	{
		memcpy(out, addr, size);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	--g_inOurGuard;
	return ok;
}

static bool ReadRva(uintptr_t base, unsigned long rva, void* out, size_t size)
{
	if (!base || !rva)
		return false;
	return ReadBytesGuarded((const void*)(base + rva), out, size);
}

// Walks throw -> ThrowInfo -> CatchableTypeArray -> first CatchableType ->
// TypeDescriptor. Every field in that chain but the descriptor's name is a
// 32-bit image-relative offset from the module base the throw carries.
static bool CaptureTypeDescriptorName(const EXCEPTION_RECORD* er, char* out, size_t cap)
{
	uintptr_t base = (uintptr_t)er->ExceptionInformation[3];
	const void* throwInfo = (const void*)er->ExceptionInformation[2];
	if (!base || !throwInfo)
		return false;

	unsigned long fields[4];
	if (!ReadBytesGuarded(throwInfo, fields, sizeof(fields)))
		return false;
	unsigned long catchableArrayRva = fields[3];

	unsigned long arrayHead[2];   // count, then the first entry's offset
	if (!ReadRva(base, catchableArrayRva, arrayHead, sizeof(arrayHead)))
		return false;
	if ((long)arrayHead[0] <= 0)
		return false;

	unsigned long catchable[2];   // properties, then the descriptor's offset
	if (!ReadRva(base, arrayHead[1], catchable, sizeof(catchable)))
		return false;

	// TypeDescriptor: vftable pointer, spare pointer, then the name.
	char raw[TYPE_CAP];
	if (!ReadRva(base, catchable[1] + 16, raw, sizeof(raw)))
		return false;
	raw[TYPE_CAP - 1] = '\0';

	return FatalDemangleTypeName(raw, out, cap) > 0;
}

} // namespace
using namespace cpp_exception_detail;

void CppExceptionNote(const void* exceptionRecord)
{
	const EXCEPTION_RECORD* er = (const EXCEPTION_RECORD*)exceptionRecord;
	if (!er)
		return;

	InterlockedIncrement(&g_count);
	InterlockedExchange64(&g_lastAddr, (LONG64)(uintptr_t)er->ExceptionAddress);

	if (!IsThrowWithTypeInfo(er))
		return;

	// The same ThrowInfo is the same type, and repeated throws are nearly all
	// repeats of one site. Memoising it bounds the descriptor walk to once per
	// distinct site, so a process that throws constantly costs two interlocked
	// increments per throw instead of four guarded reads.
	LONG64 throwInfo = (LONG64)(uintptr_t)er->ExceptionInformation[2];
	if (InterlockedCompareExchange64(&g_lastThrowInfo, 0, 0) == throwInfo)
		return;

	char name[TYPE_CAP];
	if (!CaptureTypeDescriptorName(er, name, sizeof(name)))
		return;

	// Publish the new length after the bytes; a concurrent diagnostic reader
	// still copies the live array and can see mixed bytes during a rewrite.
	size_t len = 0;
	while (len + 1 < TYPE_CAP && name[len])
	{
		g_typeName[len] = name[len];
		++len;
	}
	g_typeName[len] = '\0';
	InterlockedExchange(&g_typeLen, (LONG)len);
	InterlockedExchange64(&g_lastThrowInfo, throwInfo);
}

long CppExceptionCount()
{
	return InterlockedCompareExchange(&g_count, 0, 0);
}

size_t CppExceptionLastType(char* out, size_t cap)
{
	if (!out || cap == 0)
		return 0;
	out[0] = '\0';
	size_t len = (size_t)InterlockedCompareExchange(&g_typeLen, 0, 0);
	size_t n = 0;
	while (n < len && n + 1 < cap && g_typeName[n])
	{
		out[n] = g_typeName[n];
		++n;
	}
	out[n] = '\0';
	return n;
}

unsigned __int64 CppExceptionLastAddress()
{
	return (unsigned __int64)InterlockedCompareExchange64(&g_lastAddr, 0, 0);
}

long CppExceptionClaimRecordSlot(double nowSec)
{
	for (;;)
	{
		LONG written = InterlockedCompareExchange(&g_recordSeq, 0, 0);
		if (!CppRecordAdmit((long)written, nowSec, g_lastWriteSec))
			return 0;
		if (InterlockedCompareExchange(&g_recordSeq, written + 1, written) == written)
		{
			// Plain store: a race here only shifts one throw's spacing by one
			// slot, and the claim itself is already serialised above.
			g_lastWriteSec = nowSec;
			return written + 1;
		}
	}
}

size_t CppExceptionToken(char* out, size_t cap)
{
	if (!out || cap == 0)
		return 0;
	out[0] = '\0';

	long count = CppExceptionCount();
	size_t n = 0;

	const char* head = "cppEx=";
	while (*head && n + 1 < cap)
		out[n++] = *head++;

	char digits[16];
	int d = 0;
	unsigned long v = (unsigned long)count;
	if (v == 0)
		digits[d++] = '0';
	while (v > 0 && d < 16)
	{
		digits[d++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (d > 0 && n + 1 < cap)
		out[n++] = digits[--d];
	out[n] = '\0';

	if (count > 0)
	{
		char name[TYPE_CAP];
		CppExceptionLastType(name, sizeof(name));
		const char* open = "(last=";
		while (*open && n + 1 < cap)
			out[n++] = *open++;
		const char* p = name[0] ? name : "?";
		while (*p && n + 1 < cap)
			out[n++] = *p++;

		const char* at = " @0x";
		while (*at && n + 1 < cap)
			out[n++] = *at++;
		unsigned __int64 addr = CppExceptionLastAddress();
		static const char HEX[] = "0123456789ABCDEF";
		for (int i = 15; i >= 0 && n + 1 < cap; --i)
			out[n++] = HEX[(size_t)((addr >> (i * 4)) & 0xF)];

		if (n + 1 < cap)
			out[n++] = ')';
		out[n] = '\0';
	}

	return n;
}
