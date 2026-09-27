#include "diag/mem_format.h"

namespace mem_format_detail {

// Bounded appenders. Each keeps the buffer NUL-terminated and stops at the
// cap rather than truncating mid-write into an unterminated buffer.
void AppendChar(char* buf, size_t cap, size_t* n, char c)
{
	if (cap == 0)
		return;
	if (*n + 1 < cap)
	{
		buf[*n] = c;
		++(*n);
		buf[*n] = '\0';
	}
}

void AppendStr(char* buf, size_t cap, size_t* n, const char* s)
{
	while (s && *s)
		AppendChar(buf, cap, n, *s++);
}

void AppendDec(char* buf, size_t cap, size_t* n, unsigned __int64 v)
{
	char tmp[24];
	int len = 0;
	if (v == 0)
		tmp[len++] = '0';
	while (v > 0 && len < 24)
	{
		tmp[len++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (len > 0)
		AppendChar(buf, cap, n, tmp[--len]);
}

// key=<value in MB> when the group was read, key=- when it was not.
void AppendMB(char* buf, size_t cap, size_t* n, const char* key,
              unsigned __int64 bytes, int have)
{
	AppendStr(buf, cap, n, key);
	AppendChar(buf, cap, n, '=');
	if (have)
		AppendDec(buf, cap, n, MemBytesToMB(bytes));
	else
		AppendChar(buf, cap, n, '-');
}

} // namespace
using namespace mem_format_detail;

void MemFiguresClear(MemFigures* f)
{
	if (!f)
		return;
	f->privateCommit = 0;
	f->peakPrivate = 0;
	f->workingSet = 0;
	f->pagefileFree = 0;
	f->pagefileTotal = 0;
	f->memLoadPct = 0;
	f->haveProcess = 0;
	f->haveSystem = 0;
}

unsigned long MemBytesToMB(unsigned __int64 bytes)
{
	const unsigned __int64 MB = 1024ULL * 1024ULL;
	return (unsigned long)((bytes + MB / 2) / MB);
}

size_t MemFormatLong(char* buf, size_t cap, const MemFigures& f)
{
	size_t n = 0;
	if (cap > 0)
		buf[0] = '\0';

	AppendMB(buf, cap, &n, "privMB", f.privateCommit, f.haveProcess);
	AppendChar(buf, cap, &n, ' ');
	AppendMB(buf, cap, &n, "peakPrivMB", f.peakPrivate, f.haveProcess);
	AppendChar(buf, cap, &n, ' ');
	AppendMB(buf, cap, &n, "wsMB", f.workingSet, f.haveProcess);
	AppendChar(buf, cap, &n, ' ');
	AppendMB(buf, cap, &n, "sysCommitFreeMB", f.pagefileFree, f.haveSystem);
	AppendChar(buf, cap, &n, ' ');
	AppendMB(buf, cap, &n, "sysCommitTotalMB", f.pagefileTotal, f.haveSystem);
	AppendStr(buf, cap, &n, " sysLoad=");
	if (f.haveSystem)
	{
		AppendDec(buf, cap, &n, (unsigned __int64)f.memLoadPct);
		AppendChar(buf, cap, &n, '%');
	}
	else
		AppendChar(buf, cap, &n, '-');

	return n;
}

size_t MemFormatShort(char* buf, size_t cap, const MemFigures& f)
{
	size_t n = 0;
	if (cap > 0)
		buf[0] = '\0';

	AppendStr(buf, cap, &n, "priv");
	if (f.haveProcess)
	{
		AppendDec(buf, cap, &n, MemBytesToMB(f.privateCommit));
		AppendStr(buf, cap, &n, "MB");
	}
	else
		AppendChar(buf, cap, &n, '-');

	AppendStr(buf, cap, &n, "/free");
	if (f.haveSystem)
	{
		AppendDec(buf, cap, &n, MemBytesToMB(f.pagefileFree));
		AppendStr(buf, cap, &n, "MB");
	}
	else
		AppendChar(buf, cap, &n, '-');

	AppendStr(buf, cap, &n, "/load");
	if (f.haveSystem)
	{
		AppendDec(buf, cap, &n, (unsigned __int64)f.memLoadPct);
		AppendChar(buf, cap, &n, '%');
	}
	else
		AppendChar(buf, cap, &n, '-');

	return n;
}
