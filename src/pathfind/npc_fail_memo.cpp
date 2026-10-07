// npc_fail_memo.cpp - the NPC failed-search memo's path-thread half, the door-state pass-through and
// the main-thread tick. The table, the door count and the window counters are written on the path
// thread (the door detour runs there too, in the pass's message drain); the mode and the reset
// generation are written on the main thread. The path-thread calls take no lock, allocate nothing
// and log nothing.
#include "pathfind/npc_fail_memo.h"

#ifdef KEO_DEBUG
#include "pathfind/npc_cap_requester.h"
#include "pathfind/pathfind_config.h"
#include "pathfind/astar_cost.h"
#include "plugin/hook_manifest.h"
#include "base/core.h"
#include <sstream>
#include <iomanip>

static NfmTable          s_table;              // path thread
static unsigned          s_lastSections = 0;   // path thread
static bool              s_haveSections = false;
static volatile LONG     s_mode = NFM_OFF;     // main writes, path reads
static volatile LONG     s_resetGen = 0;       // main
static volatile LONG     s_doorEpoch = 0;      // path
static bool              s_doorSeen = false;   // set once at install
static LONGLONG          s_qpf = 0;            // set once at install

// Window counters: the path thread adds, the main thread reads and zeroes them for each line.
static volatile LONG   s_hit = 0, s_miss = 0, s_ins = 0, s_agree = 0, s_wrong = 0, s_wrongCluster = 0;
static volatile LONG   s_insU = 0, s_hitU = 0, s_wrongClass = 0;
static volatile LONG   s_byFace = 0, s_skip = 0, s_entries = 0;
static volatile LONG   s_bumpSections = 0, s_bumpDoor = 0, s_bumpReset = 0, s_bumpTtl = 0;
static volatile LONG64 s_savedTicks = 0;

static int    s_tickMode = NFM_OFF;   // main thread
static bool   s_wasLoading = false;
static double s_lastLine = -1.0e9;

typedef void (*ChangeDoorState_t)(void* navMesh, const void* box, bool open, void* instance);
static ChangeDoorState_t orig_changeDoorState = NULL;

static void hook_changeDoorState(void* navMesh, const void* box, bool open, void* instance)
{
	orig_changeDoorState(navMesh, box, open, instance);
	InterlockedIncrement(&s_doorEpoch);
	InterlockedIncrement(&s_bumpDoor);
}

static LONGLONG NowTicks()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

bool NpcFailMemoBefore(void* collection, void* input, void* output, AstarCallerClass cls,
                       bool playerTag, bool waved, NpcFailMemoCall* call)
{
	call->mode = NFM_OFF;
	call->wouldHit = 0;
	call->exactFace = 0;
	call->recordedStatus = 0;
	call->unreachableCovered = 0;
	int mode = (int)s_mode;
	if (!NfmCovers(mode, cls, playerTag, waved) || !collection || !input || !output)
		return false;
	const unsigned char* in = (const unsigned char*)input;
	const unsigned* goals = *(const unsigned* const*)(in + NFM_IN_GOAL_KEYS);
	int goalCount = *(const int*)(in + NFM_IN_GOAL_KEYS + 8);
	bool ok = false;
	unsigned sections = NfmSectionsFingerprint(collection, &ok);
	if (!goals || goalCount != 1 || !ok)
	{
		InterlockedIncrement(&s_skip);
		return false;
	}
	if (s_haveSections && sections != s_lastSections)
		InterlockedIncrement(&s_bumpSections);
	s_lastSections = sections;
	s_haveSections = true;

	unsigned startFace = *(const unsigned*)(in + NFM_IN_START_FACE);
	int cluster = -1, data = -1;
	NfmReadFace(collection, startFace, &cluster, &data);
	const unsigned char* modifier = *(const unsigned char* const*)(in + NFM_IN_COST_MOD);
	call->key = NfmMakeKey(goals[0], startFace, cluster, *(const unsigned*)(in + NFM_IN_AGENT_DIAM),
	                       modifier != NULL, modifier ? *(const unsigned*)(modifier + NFM_MOD_WATER_COST) : 0u);
	call->unreachableCovered = NfmUnreachableInputCovered(input) ? 1 : 0;
	if (call->key.flags & NFM_START_FACE)
		InterlockedIncrement(&s_byFace);
	call->startFace = startFace;
	call->epoch.sections = sections;
	call->epoch.door = s_doorEpoch;
	call->epoch.reset = s_resetGen;
	call->mode = mode;

	NfmLookup look = NfmFind(&s_table, call->key, startFace, call->epoch, NowTicks(),
	                         NfmTtlTicks(s_qpf, s_doorSeen));
	if (look.dropped[NFM_DROP_TTL])
		InterlockedExchangeAdd(&s_bumpTtl, look.dropped[NFM_DROP_TTL]);
	InterlockedExchange(&s_entries, s_table.entries);
	if (look.index < 0 || (s_table.e[look.index].status == 2 && !call->unreachableCovered))
	{
		InterlockedIncrement(&s_miss);
		return false;
	}
	const NfmEntry& entry = s_table.e[look.index];
	InterlockedIncrement(&s_hit);
	if (entry.status == 2)
		InterlockedIncrement(&s_hitU);
	if (mode == NFM_OBSERVE)
	{
		call->wouldHit = 1;
		call->exactFace = look.exactFace;
		call->recordedStatus = entry.status;
		return false;
	}
	NfmReplay(entry, output);
	InterlockedExchangeAdd64(&s_savedTicks, s_table.e[look.index].serviceTicks);
	return true;
}

static bool CharacterClass(AstarCallerClass cls)
{
	return cls == ASTAR_CALLER_CHARACTER_PLAYER || cls == ASTAR_CALLER_CHARACTER_NPC
	    || cls == ASTAR_CALLER_CHARACTER_UNKNOWN;
}

void NpcFailMemoAfter(const NpcFailMemoCall* call, void* collection, void* input, AstarCallerClass cls,
                      int status, int cause, int iterations, long long ticks, const void* request,
                      int* goalData, int* startCluster)
{
	*goalData = -1;
	*startCluster = -1;
	if (status == 3 && cause == 3 && CharacterClass(cls) && collection && input)
	{
		const unsigned char* in = (const unsigned char*)input;
		const unsigned* goals = *(const unsigned* const*)(in + NFM_IN_GOAL_KEYS);
		int goalCount = *(const int*)(in + NFM_IN_GOAL_KEYS + 8);
		unsigned startFace = *(const unsigned*)(in + NFM_IN_START_FACE);
		int cluster = -1, unused = -1;
		if (goals && goalCount >= 1)
			NfmReadFace(collection, goals[0], &unused, goalData);
		if (NfmReadFace(collection, startFace, &cluster, &unused))
			*startCluster = (int)NfmClusterKey(startFace, cluster);
		NpcCapRequesterNote(request, cls == ASTAR_CALLER_CHARACTER_PLAYER ? 1 : 0);
	}
	if (call->mode == NFM_OFF)
		return;
	if (call->wouldHit)
	{
		if (NfmWrong(status))
		{
			InterlockedIncrement(&s_wrong);
			if (!call->exactFace)
				InterlockedIncrement(&s_wrongCluster);
			NfmErase(&s_table, call->key);
		}
		else
		{
			InterlockedIncrement(&s_agree);
			if (NfmWrongClass(call->recordedStatus, status))
				InterlockedIncrement(&s_wrongClass);
			InterlockedExchangeAdd64(&s_savedTicks, ticks);
		}
	}
	if (NfmShouldInsert(status, cause, iterations) && (status != 2 || call->unreachableCovered))
	{
		NfmInsert(&s_table, call->key, call->startFace, call->epoch, NowTicks(), ticks, status, cause);
		InterlockedIncrement(&s_ins);
		if (status == 2)
			InterlockedIncrement(&s_insU);
	}
	InterlockedExchange(&s_entries, s_table.entries);
}

static const char* ModeName(int mode)
{
	return mode == NFM_ON ? "on" : mode == NFM_OBSERVE ? "observe" : "off";
}

static void PrintMemoLine()
{
	LONG hit = InterlockedExchange(&s_hit, 0);
	LONG miss = InterlockedExchange(&s_miss, 0);
	LONG ins = InterlockedExchange(&s_ins, 0);
	LONG agree = InterlockedExchange(&s_agree, 0);
	LONG wrong = InterlockedExchange(&s_wrong, 0);
	LONG wrongCluster = InterlockedExchange(&s_wrongCluster, 0);
	LONG byFace = InterlockedExchange(&s_byFace, 0);
	LONG skip = InterlockedExchange(&s_skip, 0);
	LONG bumpSections = InterlockedExchange(&s_bumpSections, 0);
	LONG bumpDoor = InterlockedExchange(&s_bumpDoor, 0);
	LONG bumpReset = InterlockedExchange(&s_bumpReset, 0);
	LONG bumpTtl = InterlockedExchange(&s_bumpTtl, 0);
	LONGLONG saved = InterlockedExchange64(&s_savedTicks, 0);
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1);
	ss << "NpcFailMemo: mode=" << ModeName(s_tickMode) << " hit=" << hit << " miss=" << miss << " ins=" << ins
	   << " agree=" << agree << " wrong=" << wrong << " wrongCluster=" << wrongCluster
	   << " savedMs=" << (s_qpf > 0 ? (double)saved * 1000.0 / (double)s_qpf : 0.0)
	   << " entries=" << InterlockedCompareExchange(&s_entries, 0, 0)
	   << " bumps(sect/door/reset/ttl)=" << bumpSections << "/" << bumpDoor << "/" << bumpReset << "/" << bumpTtl
	   << " byFace=" << byFace << " skip=" << skip
	   << " door=" << (s_doorSeen ? "on" : "refused")
	   << " ttl=" << (s_doorSeen ? NFM_TTL_SECONDS : NFM_TTL_NO_DOOR_SECONDS) << "s"
	   << " findPathFull=" << (AstarCostHookInstalled() ? "on" : "off")
	   << " insU=" << InterlockedExchange(&s_insU, 0)
	   << " hitU=" << InterlockedExchange(&s_hitU, 0)
	   << " wrongClass=" << InterlockedExchange(&s_wrongClass, 0);
	LogMsg(ss.str());
}

void NpcFailMemoTick(double now, bool saveLoading)
{
	int want = pathfind::g_pathfindCfg.npcFailMemoMode;
	if (want < NFM_OFF || want > NFM_ON)
		want = NFM_OFF;
	bool loadEdge = saveLoading && !s_wasLoading;
	s_wasLoading = saveLoading;
	// While off no covered search refreshes the published count, and a search already past the
	// check can still publish one after the switch, so the count is zeroed every frame. The table
	// itself stays: the reset generation retires its entries when a covered search next meets them.
	if (want == NFM_OFF)
		InterlockedExchange(&s_entries, 0);
	if (want != s_tickMode || loadEdge)
	{
		InterlockedIncrement(&s_resetGen);
		InterlockedIncrement(&s_bumpReset);
	}
	if (want != s_tickMode)
	{
		s_tickMode = want;
		InterlockedExchange(&s_mode, want);
		s_lastLine = now;
		PrintMemoLine();
		return;
	}
	if (now - s_lastLine < 10.0)
		return;
	s_lastLine = now;
	PrintMemoLine();
	NpcCapRequesterPrintLine();
}

void InstallNpcFailMemo(int* installed, int*)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	s_qpf = f.QuadPart;
	NfmClear(&s_table);
	const char* why = "not wanted";
	if (HookRowWanted(HOOK_NAVMESH_CHANGE_DOOR_STATE))
		why = HookInstall(HOOK_NAVMESH_CHANGE_DOOR_STATE, hook_changeDoorState, &orig_changeDoorState, installed, true);
	s_doorSeen = (why == NULL);
	std::ostringstream ss;
	ss << "NpcFailMemo: install door=";
	if (s_doorSeen)
		ss << "ok";
	else
		ss << "refused(" << why << ")";
	ss << " ttl=" << (s_doorSeen ? NFM_TTL_SECONDS : NFM_TTL_NO_DOOR_SECONDS) << "s table=" << NFM_TABLE_SIZE;
	LogMsg(ss.str());
}

#else

void NpcFailMemoTick(double, bool) {}
void InstallNpcFailMemo(int*, int*) {}

#endif // KEO_DEBUG
