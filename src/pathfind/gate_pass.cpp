#include "pathfind/gate_pass.h"
#include "pathfind/gate_loop.h"
#include "base/core.h"
#include "game/game.h"
#include "zone/transition.h"
#include <intrin.h>
#include <string.h>
#include <sstream>
#include <iomanip>
#pragma intrinsic(_ReturnAddress)

gatesFindPath_t orig_gatesFindPath = NULL;

struct GatePassRecord
{
	volatile LONG seq;            // record index once complete; -1 while written
	LONGLONG entry, firstSearch, exit;
	int      gates;
	LONG     genEntry, genExit;
	unsigned char inTxEntry, inTxExit, pendingEntry, pendingExit;
	int      n[4];
	LONGLONG ticks[4];
	int      iterLimit[3], stateFull[3];
};

static const int GP_RING = 64;
static GatePassRecord s_ring[GP_RING];
static volatile LONG  s_written = 0;      // records published
static LONG           s_printed = 0;      // main thread only
static LONG           s_lost    = 0;      // main thread only

// The pass in progress. Only the path thread runs the gate pass, and never two
// at once, so the record and the gates object are plain statics.
static GatePassRecord s_cur;
static void*          s_gatesObj = NULL;
static __declspec(thread) int t_gateLoop = -1;   // loop of the search in progress

static volatile LONG s_otherRva = 0;             // first unrecognised return RVA

struct Dismissal { volatile LONG seq; LONG gen; LONGLONG qpc; unsigned char onMain; };
static const int GP_DIS = 8;
static Dismissal     s_dis[GP_DIS];
static volatile LONG s_disCount = 0;

void GatePassBegin(void* gatesObj, LONGLONG entryQpc)
{
	memset((void*)&s_cur, 0, sizeof(s_cur));
	s_gatesObj         = gatesObj;
	s_cur.entry        = entryQpc;
	s_cur.gates        = -1;
	s_cur.genEntry     = TransitionGeneration();
	s_cur.inTxEntry    = isTransitionActive ? 1 : 0;
	s_cur.pendingEntry = InterlockedCompareExchange(&transitionEndPending, 0, 0) ? 1 : 0;
}

bool hook_gatesFindPath(__int64 input, __int64 fromEntry, __int64 toEntry)
{
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);
	if (s_cur.firstSearch == 0)
	{
		s_cur.firstSearch = t0.QuadPart;
		if (s_gatesObj)
			s_cur.gates = *(const int*)((const char*)s_gatesObj + 104);
	}
	unsigned long long rva = (unsigned long long)((uintptr_t)_ReturnAddress() - gameBase);
	int loop = GateLoopFromReturnRva(rva);
	if (loop == GATE_LOOP_OTHER)
		InterlockedCompareExchange(&s_otherRva, (LONG)rva, 0);

	t_gateLoop = loop;
	bool r = orig_gatesFindPath(input, fromEntry, toEntry);
	t_gateLoop = -1;

	QueryPerformanceCounter(&t1);
	s_cur.n[loop]++;
	s_cur.ticks[loop] += t1.QuadPart - t0.QuadPart;
	return r;
}

void GatePassNoteCause(int cause)
{
	int loop = t_gateLoop;
	if (loop < 0 || loop > GATE_LOOP_FIX)
		return;
	if (cause == 1) s_cur.iterLimit[loop]++;
	if (cause == 3) s_cur.stateFull[loop]++;
}

void GatePassEnd(LONGLONG exitQpc)
{
	s_cur.exit        = exitQpc;
	s_cur.genExit     = TransitionGeneration();
	s_cur.inTxExit    = isTransitionActive ? 1 : 0;
	s_cur.pendingExit = InterlockedCompareExchange(&transitionEndPending, 0, 0) ? 1 : 0;

	LONG idx = s_written;                 // single writer
	GatePassRecord* slot = &s_ring[idx % GP_RING];
	InterlockedExchange(&slot->seq, -1);
	s_cur.seq = -1;                       // the copy must not publish a stale index
	memcpy((void*)slot, (const void*)&s_cur, sizeof(GatePassRecord));
	InterlockedExchange(&slot->seq, idx);
	InterlockedIncrement(&s_written);
	s_gatesObj = NULL;
}

void GatePassNoteDismissal(LONGLONG qpc, LONG gen, bool onMain)
{
	LONG idx = InterlockedIncrement(&s_disCount) - 1;
	Dismissal* d = &s_dis[idx % GP_DIS];
	InterlockedExchange(&d->seq, -1);
	d->gen = gen;
	d->qpc = qpc;
	d->onMain = onMain ? 1 : 0;
	InterlockedExchange(&d->seq, idx);
}

// A consistent copy of record `idx`, or false when it was overwritten.
static bool ReadRecord(LONG idx, GatePassRecord* out)
{
	const GatePassRecord* slot = &s_ring[idx % GP_RING];
	if (InterlockedCompareExchange((volatile LONG*)&slot->seq, 0, 0) != idx)
		return false;
	memcpy(out, (const void*)slot, sizeof(GatePassRecord));
	return InterlockedCompareExchange((volatile LONG*)&slot->seq, 0, 0) == idx;
}

static bool FindDismissal(LONG gen, Dismissal* out)
{
	LONG count = InterlockedCompareExchange(&s_disCount, 0, 0);
	for (LONG i = count - 1; i >= 0 && i >= count - GP_DIS; --i)
	{
		const Dismissal* d = &s_dis[i % GP_DIS];
		if (InterlockedCompareExchange((volatile LONG*)&d->seq, 0, 0) != i)
			continue;
		Dismissal copy = *d;
		if (InterlockedCompareExchange((volatile LONG*)&d->seq, 0, 0) == i && copy.gen == gen)
		{
			*out = copy;
			return true;
		}
	}
	return false;
}

static void PrintRecord(const GatePassRecord& r)
{
	static const char* names[3] = { "enabled", "interior", "fix" };
	double totalMs = QpcToMs(r.exit - r.entry);
	double handshakeMs = QpcToMs((r.firstSearch ? r.firstSearch : r.exit) - r.entry);

	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1)
	   << "GatePass: gates=" << r.gates << " total=" << totalMs << "ms handshake=" << handshakeMs << "ms";
	for (int i = 0; i < 3; ++i)
		ss << " " << names[i] << "=" << r.n[i] << "/" << QpcToMs(r.ticks[i]) << "ms";
	ss << " other=" << r.n[GATE_LOOP_OTHER]
	   << " iterLimit=" << r.iterLimit[0] << "/" << r.iterLimit[1] << "/" << r.iterLimit[2]
	   << " stateFull=" << r.stateFull[0] << "/" << r.stateFull[1] << "/" << r.stateFull[2]
	   << " inTx=" << (int)r.inTxEntry << ">" << (int)r.inTxExit
	   << " pending=" << (int)r.pendingEntry << ">" << (int)r.pendingExit
	   << " gen=" << r.genEntry << ">" << r.genExit;
	Dismissal d;
	if (FindDismissal(r.genEntry, &d))
		ss << " dismissDelta=" << std::showpos << QpcToMs(d.qpc - r.exit) << std::noshowpos
		   << "ms(" << (d.onMain ? "main" : "path") << ")";
	else
		ss << " dismissDelta=-";
	if (s_lost)
	{
		ss << " lost=" << s_lost;
		s_lost = 0;
	}
	LogMsg(ss.str());
}

void GatePassTickMain()
{
	LONG other = InterlockedExchange(&s_otherRva, 0);
	if (other)
	{
		char line[96];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"GatePass: unrecognised Gates__findPath return RVA 0x%lX", (unsigned long)other);
		LogMsg(line);
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	LONG written = InterlockedCompareExchange(&s_written, 0, 0);
	if (written - s_printed > GP_RING)
	{
		s_lost += written - s_printed - GP_RING;
		s_printed = written - GP_RING;
	}
	while (s_printed < written)
	{
		GatePassRecord r;
		if (!ReadRecord(s_printed, &r))
		{
			++s_lost;
			++s_printed;
			continue;
		}
		if (QpcToMs(now.QuadPart - r.exit) < 2000.0)
			break;                        // wait for a dismissal that may still follow it
		PrintRecord(r);
		++s_printed;
	}
}

std::string GatePassTransitionToken(LONG gen, LONGLONG endQpc)
{
	int inBracket = 0, preDismiss = 0;
	LONGLONG lastExit = 0;
	LONG written = InterlockedCompareExchange(&s_written, 0, 0);
	for (LONG i = written - 1; i >= 0 && i >= written - GP_RING; --i)
	{
		GatePassRecord r;
		if (!ReadRecord(i, &r) || r.genEntry != gen || !r.inTxEntry)
			continue;
		++inBracket;
		if (r.exit <= endQpc)
		{
			++preDismiss;
			if (r.exit > lastExit) lastExit = r.exit;
		}
	}
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1)
	   << " gatePasses=" << inBracket << " preDismiss=" << preDismiss << " lastPassToDismiss=";
	if (lastExit)
		ss << QpcToMs(endQpc - lastExit) << "ms";
	else
		ss << "-";
	return ss.str();
}
