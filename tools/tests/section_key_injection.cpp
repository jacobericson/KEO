// Injection harness: fault deliberately after a scan and show that the ring
// reaches the record, with two readings a reader can tell apart.
//
// Links the shipped section_key_scan.cpp / section_key_ring.cpp / policy
// unchanged, stubs only the two core.h externs they use, and plays the part
// the crash handler plays in the game: an exception filter formats the ring
// into a fixed buffer and writes it to a file, which the harness reads back.
//
// Not part of build_tests.bat: it builds against KenshiLib's include path and
// faults on purpose. tools\tests\run_injections.bat builds and runs it (its row in injections.txt).

#define ZONEOPT_DEBUG 1
#include "fixes/streaming/section_key_ring.h"
#include "fixes/streaming/section_key_scan.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

// --- the two core.h externs the linked sources need ---
__declspec(thread) int g_inOurGuard = 0;
void LogMsgDeferrable(const char* line) { std::printf("[deferred] %s\n", line); }

// --- a stand-in streaming collection and clearance context ---

struct FakeInstanceInfo          // 48 bytes, as the engine's is
{
	void* instancePtr;
	void* volumeInstancePtr;
	void* clusterGraphInstance;
	void* mediator;
	void* volumeMediator;
	unsigned __int64 treeNode;
};

struct FakeCollection
{
	unsigned __int64 head[4];    // whatever precedes the array
	FakeInstanceInfo* data;      // +32
	int size;                    // +40
	int capacity;
};

struct FakeArray                 // hkArray<int>
{
	int* data;
	int  size;
	int  capacity;
};

struct FakeContext
{
	unsigned char pad0[56];
	FakeCollection* collection;  // +56
	unsigned char pad1[16];      // 64..79
	int*          pendingData;   // +80
	int           pendingCount;  // +88
	unsigned char pad2[97];      // 92..188
	unsigned char method;        // +189
	unsigned char pad3[6];
};

static unsigned int PackKey(unsigned int section, unsigned int element)
{
	return (section << 22) | (element & 0x3FFFFFu);
}

static const int kSections = 8;
static FakeInstanceInfo g_slots[kSections];
static char g_meshes[kSections][64];

static void BuildCollection(FakeCollection* coll)
{
	for (int i = 0; i < kSections; ++i)
	{
		std::memset(&g_slots[i], 0, sizeof(g_slots[i]));
		g_slots[i].instancePtr = g_meshes[i];
	}
	std::memset(coll, 0, sizeof(*coll));
	coll->data = g_slots;
	coll->size = kSections;
	coll->capacity = kSections;
}

// The record write, in the shape the guard uses: format into a fixed buffer
// inside the filter, then write the file.
static char g_record[2048];
static DWORD g_recordLen = 0;

static LONG RecordFilter(EXCEPTION_POINTERS* ep)
{
	(void)ep;
	size_t n = SectionKeyRingFormat(g_record, sizeof(g_record), 2);
	g_recordLen = (DWORD)n;
	return EXCEPTION_EXECUTE_HANDLER;
}

// The fault: a read through a pointer the scan has already classified, which
// is what the engine's unchecked lookup does with a bad slot.
static void Victim()
{
	volatile int* wild = (volatile int*)(uintptr_t)0x7408687BC;
	int v = *wild;
	std::printf("unreachable %d\n", v);
}

static void RunCase(const char* label, bool outOfRange, const char* outPath)
{
	SectionKeyRingInit();

	FakeCollection coll;
	BuildCollection(&coll);

	int callerKeys[3];
	callerKeys[0] = (int)PackKey(1, 113);
	callerKeys[1] = (int)PackKey(3, 7);
	// The one difference between the two runs: a section index past the array.
	callerKeys[2] = (int)PackKey(outOfRange ? 517u : 5u, 42);

	int pendingKeys[1];
	pendingKeys[0] = (int)PackKey(2, 9);

	FakeArray arr;
	arr.data = callerKeys;
	arr.size = 3;
	arr.capacity = 3;

	FakeContext ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	ctx.collection = &coll;
	ctx.pendingData = pendingKeys;
	ctx.pendingCount = 1;
	ctx.method = 1;

	SectionKeyScanClearance(&ctx, &arr);
	SectionKeyScanCut(&coll, (unsigned int)callerKeys[2]);

	g_recordLen = 0;
	__try
	{
		Victim();
	}
	__except (RecordFilter(GetExceptionInformation()))
	{
		std::printf("%s: fault taken, record %lu bytes\n", label, (unsigned long)g_recordLen);
	}

	HANDLE h = CreateFileA(outPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(h, g_record, g_recordLen, &written, NULL);
		CloseHandle(h);
	}

	// Read it back, exactly as a triage pass would.
	char back[2048];
	std::memset(back, 0, sizeof(back));
	h = CreateFileA(outPath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;
	if (h != INVALID_HANDLE_VALUE)
	{
		ReadFile(h, back, sizeof(back) - 1, &got, NULL);
		CloseHandle(h);
	}
	std::printf("---- %s (%s) ----\n%s\n", label, outPath, back);
}

static bool Contains(const char* path, const char* needle)
{
	char buf[2048];
	std::memset(buf, 0, sizeof(buf));
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD got = 0;
	ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
	CloseHandle(h);
	return std::strstr(buf, needle) != NULL;
}

int main()
{
	RunCase("in-range", false, "build\\tests\\skey_in_range.txt");
	RunCase("out-of-range", true, "build\\tests\\skey_out_of_range.txt");

	int failures = 0;
	struct Check { const char* path; const char* needle; bool want; const char* what; };
	const Check checks[] = {
		{ "build\\tests\\skey_in_range.txt",     "SKEY #",   true,  "in-range: an entry survived" },
		{ "build\\tests\\skey_in_range.txt",     "oob=0(",   true,  "in-range: reads oob=0" },
		{ "build\\tests\\skey_in_range.txt",     "firstOobKey", false, "in-range: names no bad key" },
		{ "build\\tests\\skey_out_of_range.txt", "SKEY #",   true,  "out-of-range: an entry survived" },
		{ "build\\tests\\skey_out_of_range.txt", "oob=0(",   false, "out-of-range: does not read oob=0" },
		{ "build\\tests\\skey_out_of_range.txt", "firstOobKey=0x81400", true,
		  "out-of-range: names the bad key" },
	};
	for (int i = 0; i < (int)(sizeof(checks) / sizeof(checks[0])); ++i)
	{
		bool got = Contains(checks[i].path, checks[i].needle);
		if (got != checks[i].want)
		{
			std::printf("FAIL: %s\n", checks[i].what);
			++failures;
		}
	}
	std::printf(failures ? "section_key_injection: %d failure(s)\n"
	                     : "section_key_injection: the two records differ where a reader looks (%d failures)\n",
	            failures);
	return failures ? 1 : 0;
}
