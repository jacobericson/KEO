// hulls_fault.cpp - First-chance hull fault writer.
// Runs on the faulting thread with no allocation and no lock; never handles the exception.

#include "hulls_detail.h"

namespace audit {
namespace audithulls_detail {

// ---- Fault record ------------------------------------------------------------
//
// A fault on the physics thread inside the delete batch leaves no anomaly if
// the hull was freed by a destructor that is not hooked, pushed dangling
// inline, or overwritten. The vectored handler below writes what the table
// and the ring know about the batch entry to <run>_hullcrash.txt, from the
// faulting thread: a stack buffer, hand formatting, WriteFile on a handle
// opened at install. It never handles the exception.

struct Out
{
	char buf[512];
	int  n;

	Out() : n(0) {}
	void Ch(char c) { if (n < (int)sizeof(buf) - 2) buf[n++] = c; }
	void Str(const char* s) { while (*s) Ch(*s++); }
	void Hex(unsigned long long v)
	{
		char t[16];
		int k = 0;
		do { t[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
		Str("0x");
		while (k) Ch(t[--k]);
	}
	void Dec(unsigned long long v)
	{
		char t[24];
		int k = 0;
		do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
		while (k) Ch(t[--k]);
	}
	// Seconds since the audit started, microsecond resolution.
	void Sec(LONG64 qpc)
	{
		if (!qpc) { Str("-"); return; }
		LONG64 d = qpc - g_qpcStart;
		if (d < 0) { Ch('-'); d = -d; }
		unsigned long long f = (unsigned long long)g_qpcFreq;
		unsigned long long us = (unsigned long long)d / f * 1000000ull +
		                        (unsigned long long)d % f * 1000000ull / f;
		Dec(us / 1000000ull);
		Ch('.');
		unsigned long long frac = us % 1000000ull;
		for (unsigned long long div = 100000ull; div; div /= 10)
			Ch((char)('0' + (frac / div) % 10));
	}
	void Addr(uintptr_t a)
	{
		if (a && a >= g_exeBase && a < g_exeEnd)
		{
			Str("exe+");
			Hex(a - g_exeBase);
		}
		else
			Hex(a);
	}
	void End()
	{
		buf[n++] = '\r';
		buf[n++] = '\n';
		DWORD written = 0;
		WriteFile(g_crashFile, buf, (DWORD)n, &written, NULL);
		n = 0;
	}
};

void OutRec(Out& o, const char* lead, const HullRec& r)
{
	bool count = r.kind == EV_FLUSH || r.kind == EV_BATCH;
	o.Str(lead);
	o.Ch('#');
	o.Dec((unsigned long long)r.seq);
	o.Str(" t=");
	o.Sec(r.qpc);
	o.Ch(' ');
	o.Str(r.kind < EV_COUNT ? EVENT_NAMES[r.kind] : "?");
	if (count)
	{
		o.Str(" n=");
		o.Dec(r.ptr);
	}
	else
	{
		o.Str(" ptr=");
		o.Hex(r.ptr);
	}
	o.Str(" tid=");
	o.Dec(r.tid);
	o.Str(" caller=");
	o.Addr(r.ret);
	o.Str(" vt=");
	o.Hex(r.vtRva);
	o.Str(" flags=");
	o.Hex(r.flags);
	o.End();
}

void OutWord(Out& o, LONG64 w)
{
	int st = StateOf(w);
	o.Str(st < ST_COUNT ? STATE_NAMES[st] : "?");
	o.Str(" extra=");
	o.Dec((unsigned long long)ExtraOf(w));
	o.Str(" lastPush=");
	o.Str(KindOf(w) < EV_COUNT && st != ST_NONE && st != ST_MADE ? EVENT_NAMES[KindOf(w)] : "-");
	o.Str(" pushCaller=");
	unsigned c = CallerOf(w);
	if (c == 0xFFFFFFFFu)
		o.Str("outside-exe");
	else if (c)
	{
		o.Str("exe+");
		o.Hex(c);
	}
	else
		o.Ch('?');
}

void OutCandidate(Out& o, const char* from, uintptr_t p)
{
	o.Str("entry ");
	o.Hex(p);
	o.Str(" (");
	o.Str(from);
	o.Str(") vt=");
	o.Hex(VtRva(p));
	Slot* s = g_table.Find(p, false);
	if (!s)
	{
		o.Str(" not in the table");
		o.End();
		return;
	}
	o.Str(" state=");
	OutWord(o, s->word);
	o.Str(" lastPushT=");
	o.Sec(s->pushQpc);
	o.Str(" lastDtorT=");
	o.Sec(s->dtorQpc);
	o.End();
	int shown = 0;
	LONG64 head = g_ringHead;
	for (LONG64 i = head > RING ? head - RING : 0; i < head && shown < 64; ++i)
	{
		HullRec r;
		if (CopyRec(i, &r) && r.ptr == p && r.kind != EV_FLUSH && r.kind != EV_BATCH)
		{
			OutRec(o, "  ", r);
			++shown;
		}
	}
}

void WriteFault(PEXCEPTION_POINTERS x)
{
	const CONTEXT& c = *x->ContextRecord;
	const EXCEPTION_RECORD& e = *x->ExceptionRecord;
	Out o;
	o.Str("hull fault (first chance) on the physics thread inside threadJunkPreBT: code=");
	o.Hex(e.ExceptionCode);
	o.Str(" at=");
	o.Addr((uintptr_t)e.ExceptionAddress);
	o.Str(" access=");
	o.Dec(e.NumberParameters > 0 ? e.ExceptionInformation[0] : 0);
	o.Str(" target=");
	o.Hex(e.NumberParameters > 1 ? e.ExceptionInformation[1] : 0);
	o.Str(" tid=");
	o.Dec(GetCurrentThreadId());
	o.Str(" t=");
	o.Sec(Now());
	o.End();

	uintptr_t ret0 = SafeField((uintptr_t)c.Rsp, 0);
	o.Str("rax=");  o.Hex(c.Rax); o.Str(" rbx="); o.Hex(c.Rbx); o.Str(" rcx="); o.Hex(c.Rcx);
	o.Str(" rdx="); o.Hex(c.Rdx); o.Str(" rsi="); o.Hex(c.Rsi); o.Str(" rdi="); o.Hex(c.Rdi);
	o.Str(" r9=");  o.Hex(c.R9);  o.Str(" rsp="); o.Hex(c.Rsp); o.Str(" [rsp]="); o.Addr(ret0);
	o.End();

	unsigned n = g_junkCount;
	const uintptr_t* data = g_junkData;
	o.Str("batch n=");
	o.Dec(n);
	o.Str(" data=");
	o.Hex((uintptr_t)data);
	o.Str(" lastConsumed=");
	o.Hex(g_lastConsumed);
	o.Str(ret0 == g_consumerRet ? " fault at the target of the loop's call (rcx = r9 = the entry, edi = its index)"
	                            : " fault is deeper than the loop's call (registers may be the callee's)");
	o.End();

	// At the loop's `call [rax]` (0x4CBF9B..0x4CBFA6): r9 = rcx = the entry
	// (loaded at 0x4CBF17 as data[rsi]), edi = its index, rsi = index * 8.
	const uintptr_t byRsi = (c.Rsi % 8 == 0 && c.Rsi / 8 < n) ? ListEntry(data, (unsigned)(c.Rsi / 8)) : 0;
	const uintptr_t byRdi = ((c.Rdi & 0xFFFFFFFFull) < n) ? ListEntry(data, (unsigned)c.Rdi) : 0;
	const uintptr_t vals[5] = { c.Rcx, c.R9, byRsi, byRdi, g_lastConsumed };
	const char* names[5] = { "rcx", "r9", "data[rsi/8]", "data[edi]", "lastConsumed" };
	uintptr_t seen[5];
	int nseen = 0;
	for (int k = 0; k < 5; ++k)
	{
		if (!PlausiblePtr(vals[k]))
			continue;
		bool dup = false;
		for (int j = 0; j < nseen; ++j)
			if (seen[j] == vals[k])
				dup = true;
		if (dup)
			continue;
		seen[nseen++] = vals[k];
		OutCandidate(o, names[k], vals[k]);
	}

	o.Str("ring tail:");
	o.End();
	LONG64 head = g_ringHead;
	for (LONG64 i = head > SNAP ? head - SNAP : 0; i < head; ++i)
	{
		HullRec r;
		if (CopyRec(i, &r))
			OutRec(o, "  ", r);
	}

	for (int k = 0; k < AN_COUNT; ++k)
	{
		LONG total = g_anomCount[k];
		if (!total)
			continue;
		o.Str("anomaly ");
		o.Str(ANOMALY_NAMES[k]);
		o.Str(" total=");
		o.Dec((unsigned long long)total);
		o.End();
		for (int j = 0; j < ANOM_KEEP; ++j)
		{
			const AnomRec& a = g_anom[k][j];
			if (!a.ready)
				continue;
			OutRec(o, "  ", a.ev);
			o.Str("    prev=");
			OutWord(o, a.prev);
			o.Str(" prevPushT=");
			o.Sec(a.prevPushQpc);
			o.Str(" prevDtorT=");
			o.Sec(a.prevDtorQpc);
			o.End();
		}
	}

	o.Str("counters push=");
	for (int k = 0; k < PUSH_KINDS; ++k)
	{
		if (k) o.Ch('/');
		o.Dec((unsigned long long)g_push[k]);
	}
	o.Str(" dtor=");
	for (int k = 0; k < DTOR_KINDS; ++k)
	{
		if (k) o.Ch('/');
		o.Dec((unsigned long long)g_dtor[k]);
	}
	o.Str(" dtorConsumer="); o.Dec((unsigned long long)g_dtorConsumer);
	o.Str(" made=");         o.Dec((unsigned long long)g_made);
	o.Str(" flushes=");      o.Dec((unsigned long long)g_flushes);
	o.Str(" batches=");      o.Dec((unsigned long long)g_batches);
	o.Str(" overflow=");     o.Dec((unsigned long long)g_table.overflow);
	o.Str(" events=");       o.Dec((unsigned long long)g_ringHead);
	o.End();
	FlushFileBuffers(g_crashFile);
}

LONG WINAPI HullFaultHandler(PEXCEPTION_POINTERS x)
{
	if (!x || !x->ExceptionRecord || !x->ContextRecord ||
	    x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (t_auditGuard || !g_inJunk || GetCurrentThreadId() != g_physTid ||
	    g_crashFile == INVALID_HANDLE_VALUE)
		return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedCompareExchange(&g_crashWritten, 1, 0) == 0)
		WriteFault(x);
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
using namespace audithulls_detail;

} // namespace audit
