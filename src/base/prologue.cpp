// prologue.cpp - Game hook prologue verification.
// Main thread at startup and the NavMesh background lazy-install thread.
// Fixed-buffer formatting uses LogMsgDeferrable: logCS on main, otherwise
// pendingLogCS for the bounded queue copy; the prologue reader takes no lock.

#include "base/core.h"
#include "game/klib_bindings.h"
#include "game/prologue_policy.h"

// =========================================================================
// Build gate — see core.h
// =========================================================================

static uintptr_t coreGameBase = 0;

void SetCoreGameBase(uintptr_t base)
{
	coreGameBase = base;
}

uintptr_t CoreGameBase()
{
	return coreGameBase;
}

// Kept standalone: MSVC 2010 rejects __try in a function that also holds
// objects needing unwinding, and everything here is POD.
static bool ReadGameBytes16(const void* addr, unsigned char* out)
{
	bool ok = true;
	GuardEnter();
	__try
	{
		memcpy(out, addr, 16);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ok = false;
	}
	GuardLeave();
	return ok;
}

// "XX XX ... XX" (16 bytes, 47 characters) into a fixed buffer. No CRT
// streams: VerifyPrologue also runs on the NavMesh bg thread (below).
static void FormatHexBytes16(char* out, size_t outSize, const unsigned char* b)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t pos = 0;
	for (int i = 0; i < 16 && pos + 3 < outSize; ++i)
	{
		if (i) out[pos++] = ' ';
		out[pos++] = HEX[(b[i] >> 4) & 0xF];
		out[pos++] = HEX[b[i] & 0xF];
	}
	if (outSize) out[pos < outSize ? pos : outSize - 1] = 0;
}

// Every line is built in a fixed buffer with _snprintf_s and handed to
// LogMsgDeferrable, never built with std::ostringstream: the NavMesh lazy
// hooks (InstallNavMeshLazyHooks) call this from the NavMesh bg thread on the
// first dispatch, where the plugin's rule is no CRT streams and no LogMsg (the
// deferral carries the line to the main thread; core.h).
bool VerifyPrologueAt(const void* addr, const unsigned char* expect, const char* name,
                      const char* where, bool* sharedOut)
{
	if (sharedOut) *sharedOut = false;

	if (expect == NULL)
		return false;

	const char* site = name ? name : "?";
	char line[DEFERRED_LOG_CHARS];

	unsigned char actual[16];
	PrologueClass result = ClassifyPrologue(actual, expect, ReadGameBytes16(addr, actual));
	if (result == PROLOGUE_UNREADABLE)
	{
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"Build gate: %s prologue unreadable at %s",
			site, where);
		LogMsgDeferrable(line);
		return false;
	}

	if (result == PROLOGUE_ORIGINAL)
		return true;

	// Another plugin loaded first and hooked this site. KenshiLib's AddHook
	// (MinHook) chains onto an existing detour correctly, so this is a pass as
	// long as the untouched tail proves the binary is still the one we know.
	if (result == PROLOGUE_SHARED)
	{
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"Build gate: %s already hooked by another plugin (detour at %s),"
			" prologue tail matches",
			site, where);
		LogMsgDeferrable(line);
		if (sharedOut) *sharedOut = true;
		return true;
	}

	char expHex[64];
	char actHex[64];
	FormatHexBytes16(expHex, sizeof(expHex), expect);
	FormatHexBytes16(actHex, sizeof(actHex), actual);
	_snprintf_s(line, sizeof(line), _TRUNCATE,
		"Build gate: %s prologue mismatch at %s\n    expected %s\n    found    %s",
		site, where, expHex, actHex);
	LogMsgDeferrable(line);
	return false;
}

bool VerifyPrologue(uintptr_t rva, const unsigned char* expect, const char* name,
                    bool* sharedOut)
{
	if (coreGameBase == 0)
	{
		if (sharedOut) *sharedOut = false;
		return false;
	}
	char where[32];
	_snprintf_s(where, sizeof(where), _TRUNCATE, "RVA 0x%llx", (unsigned long long)rva);
	return VerifyPrologueAt((const void*)KlibAddress(coreGameBase, rva), expect, name, where, sharedOut);
}
