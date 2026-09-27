#include "render/module_hooks.h"
#include "base/core.h"
#include <cstdio>
#include <cstring>

void* ResolveModuleSite(const ModuleSite& site)
{
	HMODULE m = GetModuleHandleA(site.module);
	if (!m)
		return NULL;
	if (site.symbol)
		return (void*)GetProcAddress(m, site.symbol);
	return (void*)((uintptr_t)m + site.rva);
}

bool InstallModuleHook(const ModuleSite& site, void* detour, void** orig, bool* sharedOut)
{
	if (sharedOut) *sharedOut = false;
	void* target = ResolveModuleSite(site);
	if (!target)
	{
		char line[160];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "Render gate: %s not found in %s", site.name, site.module);
		LogMsg(line);
		return false;
	}
	char where[96];
	_snprintf_s(where, sizeof(where), _TRUNCATE, "%s+0x%llx", site.module,
	            (unsigned long long)((uintptr_t)target - (uintptr_t)GetModuleHandleA(site.module)));
	if (!VerifyPrologueAt(target, site.bytes, site.name, where, sharedOut))
		return false;
	if (KenshiLib::AddHook(target, detour, orig) != KenshiLib::SUCCESS)
	{
		char line[160];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "Render gate: %s AddHook failed at %s", site.name, where);
		LogMsg(line);
		return false;
	}
	return true;
}

void* PatchImportSlot(HMODULE importer, const char* exporterDll, const char* symbol,
                      void* replacement, void* expectedCurrent)
{
	void** slot = FindImportSlot(importer, exporterDll, symbol);
	if (!slot)
		return NULL;

	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(slot, &mbi, sizeof(mbi)) != sizeof(mbi))
		return NULL;
	bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
	                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	DWORD writable = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;

	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void*), writable, &old))
		return NULL;

	void* prev;
	if (expectedCurrent)
		prev = InterlockedCompareExchangePointer(slot, replacement, expectedCurrent);
	else
		prev = InterlockedExchangePointer(slot, replacement);

	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void*), old, &ignored);

	if (expectedCurrent && prev != expectedCurrent)
		return NULL;
	return prev;
}
