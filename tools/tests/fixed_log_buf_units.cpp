// The fixed line builder against verbatim copies of the private builders it replaced: every
// form, at every buffer capacity in use, over zero, signs, widths and the capacity edge.

#include "check.h"
#include "base/fixed_log_buf.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Reference copies of the old forms, over a (b, cap, n) triple.
// ---------------------------------------------------------------------------

// The append rule and the terminator write.
static void RefChar(char* b, size_t cap, size_t* pn, char c) { if (*pn + 1 < cap) b[(*pn)++] = c; }
static void RefStr(char* b, size_t cap, size_t* pn, const char* s) { while (s && *s) RefChar(b, cap, pn, *s++); }
static void RefDone(char* b, size_t cap, size_t* pn) { RefChar(b, cap, pn, '\0'); }

// Signed decimal.
static void RefDec(char* b, size_t cap, size_t* pn, __int64 v)
{
	if (v < 0) { RefChar(b, cap, pn, '-'); v = -v; }
	char t[24]; int n = 0;
	do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 24);
	while (n) RefChar(b, cap, pn, t[--n]);
}

// Unsigned decimal.
static void RefDecU(char* b, size_t cap, size_t* pn, unsigned __int64 v)
{
	char tmp[24];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < 24)
	{
		tmp[n++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}
	while (n > 0)
		RefChar(b, cap, pn, tmp[--n]);
}

// "0x" and the fewest uppercase digits.
static void RefHex(char* b, size_t cap, size_t* pn, unsigned __int64 v)
{
	RefStr(b, cap, pn, "0x");
	char t[20]; int n = 0;
	do { int d = (int)(v & 15); t[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; }
	while (v && n < 20);
	while (n) RefChar(b, cap, pn, t[--n]);
}

// A fixed digit count, clamped to 1..16, no prefix.
static void RefHexDigits(char* b, size_t cap, size_t* pn, unsigned __int64 v, int digits)
{
	static const char* kHex = "0123456789ABCDEF";
	if (digits < 1)  digits = 1;
	if (digits > 16) digits = 16;
	for (int i = digits - 1; i >= 0; --i)
		RefChar(b, cap, pn, kHex[(size_t)((v >> (i * 4)) & 0xF)]);
}

// ---------------------------------------------------------------------------

static const unsigned __int64 kValues[] = {
	0ULL, 1ULL, (unsigned __int64)-1LL, 9ULL, 10ULL, 255ULL, 4096ULL, 0x7FFFFFFFULL,
	(unsigned __int64)-0x80000000LL, 0x123456789ABCDEF0ULL, 0xFFFFFFFFFFFFFFFFULL
};
static const int kValueCount = (int)(sizeof(kValues) / sizeof(kValues[0]));
static const int kDigitCounts[] = { 1, 8, 16, 20 };
static const int kDigitCountN = (int)(sizeof(kDigitCounts) / sizeof(kDigitCounts[0]));

static const unsigned char kFill = 0xA5;

static void CheckSame(const char* got, size_t gotN, const char* want, size_t wantN, size_t cap,
                      const char* what, unsigned __int64 v, int digits)
{
	char msg[160];
	if (digits >= 0)
		sprintf_s(msg, sizeof(msg), "cap=%u v=0x%016I64X digits=%d: %s bytes and n equal the old builder",
		        (unsigned)cap, v, digits, what);
	else
		sprintf_s(msg, sizeof(msg), "cap=%u v=0x%016I64X: %s bytes and n equal the old builder",
		        (unsigned)cap, v, what);
	Check(gotN == wantN && memcmp(got, want, cap) == 0, msg);
}

// The full line: "k=", signed then unsigned decimal, " h=", both hex forms, the terminator.
template <class B>
static void BuildLine(B* o, unsigned __int64 v)
{
	FlbStr(o, "k=");
	FlbDec(o, (__int64)v);
	FlbDecU(o, v);
	FlbStr(o, " h=");
	FlbHex(o, v);
	for (int d = 0; d < kDigitCountN; ++d)
		FlbHexDigits(o, v, kDigitCounts[d]);
	FlbDone(o);
}

static void RefLine(char* b, size_t cap, size_t* n, unsigned __int64 v)
{
	RefStr(b, cap, n, "k=");
	RefDec(b, cap, n, (__int64)v);
	RefDecU(b, cap, n, v);
	RefStr(b, cap, n, " h=");
	RefHex(b, cap, n, v);
	for (int d = 0; d < kDigitCountN; ++d)
		RefHexDigits(b, cap, n, v, kDigitCounts[d]);
	RefDone(b, cap, n);
}

// Every form on its own, then the full line, for one buffer; `o` and its storage `ob` of `cap`
// bytes are refilled before each build.
template <class B>
static void RunForms(B* o, char* ob, size_t cap, char* ref)
{
	for (int i = 0; i < kValueCount; ++i)
	{
		unsigned __int64 v = kValues[i];
		size_t rn;

		memset(ob, kFill, cap); FlbInit(o); FlbDec(o, (__int64)v); FlbDone(o);
		memset(ref, kFill, cap); rn = 0; RefDec(ref, cap, &rn, (__int64)v); RefDone(ref, cap, &rn);
		CheckSame(ob, o->n, ref, rn, cap, "FlbDec", v, -1);

		memset(ob, kFill, cap); FlbInit(o); FlbDecU(o, v); FlbDone(o);
		memset(ref, kFill, cap); rn = 0; RefDecU(ref, cap, &rn, v); RefDone(ref, cap, &rn);
		CheckSame(ob, o->n, ref, rn, cap, "FlbDecU", v, -1);

		memset(ob, kFill, cap); FlbInit(o); FlbHex(o, v); FlbDone(o);
		memset(ref, kFill, cap); rn = 0; RefHex(ref, cap, &rn, v); RefDone(ref, cap, &rn);
		CheckSame(ob, o->n, ref, rn, cap, "FlbHex", v, -1);

		for (int d = 0; d < kDigitCountN; ++d)
		{
			memset(ob, kFill, cap); FlbInit(o); FlbHexDigits(o, v, kDigitCounts[d]); FlbDone(o);
			memset(ref, kFill, cap); rn = 0; RefHexDigits(ref, cap, &rn, v, kDigitCounts[d]); RefDone(ref, cap, &rn);
			CheckSame(ob, o->n, ref, rn, cap, "FlbHexDigits", v, kDigitCounts[d]);
		}

		memset(ob, kFill, cap); FlbInit(o); BuildLine(o, v);
		memset(ref, kFill, cap); rn = 0; RefLine(ref, cap, &rn, v);
		CheckSame(ob, o->n, ref, rn, cap, "full line", v, -1);
	}
}

// A line of exactly cap - 1 characters leaves the last byte as it was: no terminator is
// written. A string past the capacity keeps its first cap - 1 bytes.
template <class B>
static void RunEdges(B* o, char* ob, size_t cap)
{
	char msg[160];

	memset(ob, kFill, cap);
	ob[cap - 1] = 'Z';
	FlbInit(o);
	for (size_t i = 0; i + 1 < cap; ++i)
		FlbChar(o, 'a');
	FlbDone(o);
	sprintf_s(msg, sizeof(msg), "cap=%u: a line of cap - 1 characters ends with n == cap - 1", (unsigned)cap);
	Check(o->n == cap - 1, msg);
	sprintf_s(msg, sizeof(msg), "cap=%u: a line of cap - 1 characters leaves b[cap - 1] unwritten", (unsigned)cap);
	Check(ob[cap - 1] == 'Z', msg);

	char longStr[1100];
	for (size_t i = 0; i < cap + 10; ++i)
		longStr[i] = (char)('a' + (int)(i % 26));
	longStr[cap + 10] = '\0';
	memset(ob, kFill, cap);
	FlbInit(o);
	FlbStr(o, longStr);
	sprintf_s(msg, sizeof(msg), "cap=%u: a string past the capacity keeps n == cap - 1", (unsigned)cap);
	Check(o->n == cap - 1, msg);
	sprintf_s(msg, sizeof(msg), "cap=%u: a string past the capacity keeps its first cap - 1 bytes", (unsigned)cap);
	Check(memcmp(ob, longStr, cap - 1) == 0, msg);
}

// A few lines against literals, so the reference copies are not the only witness.
static void CheckLiterals()
{
	FixedLogBuf o;

	FlbInit(&o); FlbStr(&o, "k="); FlbDec(&o, -1); FlbStr(&o, " u="); FlbDecU(&o, 0xFFFFFFFFFFFFFFFFULL);
	Check(strcmp(FlbDone(&o), "k=-1 u=18446744073709551615") == 0, "literal: signed -1 and unsigned max");

	FlbInit(&o); FlbDec(&o, 0); FlbChar(&o, ' '); FlbDecU(&o, 0); FlbChar(&o, ' '); FlbDec(&o, -0x80000000LL);
	Check(strcmp(FlbDone(&o), "0 0 -2147483648") == 0, "literal: zero both ways and -0x80000000");

	FlbInit(&o); FlbHex(&o, 0); FlbChar(&o, ' '); FlbHex(&o, 4096); FlbChar(&o, ' '); FlbHex(&o, 0x123456789ABCDEF0ULL);
	Check(strcmp(FlbDone(&o), "0x0 0x1000 0x123456789ABCDEF0") == 0, "literal: fewest-digit hex");

	FlbInit(&o); FlbHexDigits(&o, 255, 1); FlbChar(&o, ' '); FlbHexDigits(&o, 255, 8); FlbChar(&o, ' ');
	FlbHexDigits(&o, 0x123456789ABCDEF0ULL, 16); FlbChar(&o, ' '); FlbHexDigits(&o, 0x123456789ABCDEF0ULL, 20);
	FlbChar(&o, ' '); FlbHexDigits(&o, 0x10, 0);
	Check(strcmp(FlbDone(&o), "F 000000FF 123456789ABCDEF0 123456789ABCDEF0 0") == 0, "literal: fixed-digit hex and its clamp");
}

template <size_t N>
static void RunFixed()
{
	static FixedLogBufN<N> o;
	static char ref[N];
	RunForms(&o, o.b, N, ref);
	RunEdges(&o, o.b, N);
}

static void RunExternal(size_t cap)
{
	char storage[64];
	char ref[64];
	FlbExternal o = { storage, cap, 0 };
	RunForms(&o, storage, cap, ref);
	RunEdges(&o, storage, cap);
}

int main()
{
	Check(FlbCap((FixedLogBuf*)0) == 352, "FixedLogBuf holds 352 bytes");
	Check(sizeof(((FixedLogBuf*)0)->b) == 352, "FixedLogBuf's array is 352 bytes");

	CheckLiterals();
	RunFixed<320>();
	RunFixed<352>();
	RunFixed<384>();
	RunFixed<512>();
	RunFixed<1024>();
	RunExternal(16);
	RunExternal(64);

	return CheckExit("fixed_log_buf_units");
}
