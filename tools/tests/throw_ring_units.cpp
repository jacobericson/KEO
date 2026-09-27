// Host tests for the first-chance throw ring (src/diag/throw_ring).
//
// What they have to establish is that capturing a throw performs no I/O and
// that the record still reaches a file later. The sink is the only way text
// leaves the ring, so "the sink was not called during capture" is the whole
// claim: there is no other exit to check.
#include "diag/throw_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"


// Stands in for the file the death path opens.
struct Sink
{
	std::vector<std::string> lines;
	long calls;
	Sink() : calls(0) {}
};

static void Collect(void* ctx, const char* text, size_t len)
{
	Sink* s = (Sink*)ctx;
	s->calls++;
	s->lines.push_back(std::string(text, len));
}

static void Push(long seq, const char* type, bool afterStop)
{
	ThrowRecordFields f;
	f.seq       = seq;
	f.code      = 0xE06D7363;
	f.addr      = 0x7FFE44DD0000ULL;
	f.rva       = 0x0CD9E2;
	f.tid       = 25588;
	f.atSec     = 42;
	f.afterStop = afterStop;
	f.kind      = "CPPEX";
	f.type      = type;

	char line[THROW_RING_CHARS];
	ThrowRecordFormat(line, sizeof(line), f);
	ThrowRingPush(line);
}

static void TestCaptureDoesNoIoAndSurvivesToAFlush()
{
	ThrowRingResetForTest();

	Sink sink;
	for (long i = 1; i <= 5; ++i)
		Push(i, "Ogre::RenderingAPIException", false);

	Check(sink.calls == 0, "capturing a throw calls no sink, so it writes nothing");
	Check(ThrowRingPushed() == 5, "five throws were captured");
	Check(ThrowRingPending() == 5, "five throws are waiting for a flush");

	long delivered = ThrowRingDrain(Collect, &sink);
	Check(delivered == 5, "the flush delivers every captured throw");
	Check(sink.lines.size() == 5, "the flush writes every captured throw");
	Check(sink.lines[0].find("THROW #1:") == 0, "the oldest record comes first");
	Check(sink.lines[4].find("THROW #5:") == 0, "the newest record comes last");
	Check(sink.lines[0].find("Ogre::RenderingAPIException") != std::string::npos,
	      "the thrown type survives to the flush");

	Sink again;
	Check(ThrowRingDrain(Collect, &again) == 0, "a second flush delivers nothing");
	Check(ThrowRingPending() == 0, "nothing is pending after a flush");
}

static void TestOverrunKeepsTheNewestAndCountsTheLoss()
{
	ThrowRingResetForTest();

	const long n = THROW_RING_SLOTS + 6;
	for (long i = 1; i <= n; ++i)
		Push(i, "std::bad_alloc", false);

	Check(ThrowRingPushed() == n, "every push is counted");
	Check(ThrowRingLost() == 6, "the overwritten records are counted");
	Check(ThrowRingPending() == THROW_RING_SLOTS, "the ring holds its capacity");

	Sink sink;
	ThrowRingDrain(Collect, &sink);
	Check((long)sink.lines.size() == THROW_RING_SLOTS, "the flush delivers the ring's capacity");

	char first[32];
	sprintf_s(first, sizeof(first), "THROW #%ld:", n - THROW_RING_SLOTS + 1);
	Check(sink.lines[0].find(first) == 0, "the oldest surviving record is the newest ones' start");
	char last[32];
	sprintf_s(last, sizeof(last), "THROW #%ld:", n);
	Check(sink.lines[THROW_RING_SLOTS - 1].find(last) == 0,
	      "the throw nearest the death is kept");
}

static void TestTeardownGetsItsOwnWord()
{
	// afterStop= is reserved for a fault that landed after NavMesh::stop.
	// A caught throw during teardown must not spend that word.
	char line[THROW_RING_CHARS];
	ThrowRecordFields f;
	f.seq = 1; f.code = 0xE06D7363; f.addr = 0; f.rva = 0; f.tid = 1; f.atSec = 0;
	f.kind = "CPPEX"; f.type = "boost::thread_interrupted";

	f.afterStop = true;
	ThrowRecordFormat(line, sizeof(line), f);
	Check(std::strstr(line, "phase=afterStop") != 0, "a teardown throw says so in its own word");
	Check(std::strstr(line, "afterStop=1") == 0, "a teardown throw does not carry afterStop=1");

	f.afterStop = false;
	ThrowRecordFormat(line, sizeof(line), f);
	Check(std::strstr(line, "phase=") == 0, "an ordinary throw carries no phase token");
}

static void TestFormatTruncatesRatherThanOverruns()
{
	char small[24];
	std::memset(small, 0x7F, sizeof(small));
	ThrowRecordFields f;
	f.seq = 12345; f.code = 0xE06D7363; f.addr = ~0ULL; f.rva = ~0ULL; f.tid = 99999;
	f.atSec = 1234; f.afterStop = true; f.kind = "CPPEX";
	f.type = "a::very::long::type::name::that::does::not::fit";

	size_t n = ThrowRecordFormat(small, sizeof(small), f);
	Check(n < sizeof(small), "a truncated record stays inside the buffer");
	Check(small[n] == '\0', "a truncated record is terminated");

	// A NULL name is what a throw with no usable type information leaves.
	char line[THROW_RING_CHARS];
	f.type = 0;
	f.kind = 0;
	ThrowRecordFormat(line, sizeof(line), f);
	Check(std::strstr(line, "type=?") != 0, "a throw with no type still records as one");
}

int main()
{
	TestCaptureDoesNoIoAndSurvivesToAFlush();
	TestOverrunKeepsTheNewestAndCountsTheLoss();
	TestTeardownGetsItsOwnWord();
	TestFormatTruncatesRatherThanOverruns();

	return CheckExit("throw_ring_units");
}
