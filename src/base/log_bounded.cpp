#include "base/log_bounded.h"

bool EnterCriticalSectionBounded(CRITICAL_SECTION* cs, unsigned boundMs)
{
	const DWORD t0 = GetTickCount();
	for (;;)
	{
		if (TryEnterCriticalSection(cs))
			return true;
		if (GetTickCount() - t0 >= boundMs)
			return false;
		Sleep(1);
	}
}

bool AppendLineRaw(const char* path, const char* text, size_t len)
{
	HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const BOOL ok = WriteFile(h, text, (DWORD)len, &written, NULL) && written == (DWORD)len;
	FlushFileBuffers(h);
	CloseHandle(h);
	return ok != FALSE;
}
