#pragma once
#include <windows.h>

// The IAT slot through which `importer` calls `symbol` (a by-name import from
// `exporterDll`, compared case-insensitively), or NULL. Reads only the mapped
// image's headers, so a module loaded with DONT_RESOLVE_DLL_REFERENCES works.
void** FindImportSlot(HMODULE importer, const char* exporterDll, const char* symbol);
