#pragma once
#include <windows.h>
#include "render/pe_imports.h"

// A hook site inside a game DLL rather than the exe.
struct ModuleSite
{
	const char*   name;      // log name
	const char*   module;    // "OgreMain_x64.dll"
	const char*   symbol;    // exported name; NULL means module base + rva
	uintptr_t     rva;
	unsigned char bytes[16]; // expected prologue
};

// The site's address in the loaded module, or NULL when the module or export is missing.
void* ResolveModuleSite(const ModuleSite& site);

// Prologue-checks the site, then hooks it. False (with one log line) on any
// refusal. *sharedOut is true when another plugin had detoured the site first.
bool  InstallModuleHook(const ModuleSite& site, void* detour, void** orig, bool* sharedOut);

// Swaps an IAT slot of `importer` (an interlocked compare-and-swap when
// expectedCurrent is set, an unconditional swap when it is NULL). Returns the
// slot's previous value on success; returns NULL, with the slot left
// unchanged, when the slot can't be found or protected, or when
// expectedCurrent doesn't match what was actually there.
void* PatchImportSlot(HMODULE importer, const char* exporterDll, const char* symbol,
                      void* replacement, void* expectedCurrent);
