#include "render/pe_imports.h"
#include <cstring>

// True when [rva, rva+size) falls inside the mapped image.
static bool RvaInImage(DWORD rva, size_t size, DWORD imageSize)
{
	return rva != 0 && (DWORDLONG)rva + size <= imageSize;
}

void** FindImportSlot(HMODULE importer, const char* exporterDll, const char* symbol)
{
	if (!importer || !exporterDll || !symbol)
		return NULL;
	const unsigned char* base = (const unsigned char*)importer;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return NULL;
	DWORD imageSize = nt->OptionalHeader.SizeOfImage;
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!RvaInImage(dir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR), imageSize))
		return NULL;
	const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
	for (; RvaInImage((DWORD)((const unsigned char*)imp - base), sizeof(*imp), imageSize) && imp->Name; ++imp)
	{
		if (!RvaInImage(imp->Name, 1, imageSize))
			continue;
		if (_stricmp((const char*)(base + imp->Name), exporterDll) != 0)
			continue;
		// The lookup table (OriginalFirstThunk) keeps the by-name entries once
		// the loader has overwritten FirstThunk with resolved addresses; a
		// descriptor with no lookup table has nothing left to read names from
		// and is skipped rather than walked as if FirstThunk still held names.
		if (!RvaInImage(imp->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64), imageSize) ||
		    !RvaInImage(imp->FirstThunk, sizeof(void*), imageSize))
			continue;
		const IMAGE_THUNK_DATA64* names = (const IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
		void** slots = (void**)(base + imp->FirstThunk);
		for (;
		     RvaInImage((DWORD)((const unsigned char*)names - base), sizeof(*names), imageSize) &&
		     RvaInImage((DWORD)((const unsigned char*)slots - base), sizeof(*slots), imageSize) &&
		     names->u1.AddressOfData;
		     ++names, ++slots)
		{
			if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
				continue;
			if (!RvaInImage((DWORD)names->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME), imageSize))
				continue;
			const IMAGE_IMPORT_BY_NAME* ibn = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)ibn->Name, symbol) == 0)
				return slots;
		}
	}
	return NULL;
}
