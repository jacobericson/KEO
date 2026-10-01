// Races the coarse graph store's lock-free read against its writers. First, deterministically: a
// CgPublishOver paused right after its take reads the same entry, which must be refused. Then two
// builder-role threads publish heap blocks over 16 entries (one base blocks, one save overs,
// 200,000 publishes each) and a path-role thread posts live buffers into 16 records, while the main
// thread reads every entry in a loop, promotes the posted records and drains the retire stack
// between passes. Every accepted view's block must carry its entry's uid; any other uid is a torn
// or freed block the epoch failed to refuse. The threads yield with SwitchToThread only.
//
// Links src/planner/coarse_graph.cpp unmodified.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "planner/coarse_graph.h"

using namespace planner;

namespace coarse_graph_injection_detail
{
	const int  ENTRIES    = 16;
	const LONG PUBLISHES  = 200000;

	volatile LONG g_buildersDone = 0;
	volatile LONG g_basePublished = 0;
	volatile LONG g_overPublished = 0;
	volatile LONG g_posts = 0;
	LONG g_reads = 0, g_torn = 0;
	int  g_tornEntry = -1, g_tornUid = -1;
	TileGraph* g_tiles = NULL;   // main's array, read-only once the threads start

	// Entry d holds tile d.0, whose exterior uid is d.
	void MakeTiles()
	{
		for (int d = 0; d < ENTRIES; ++d)
		{
			TileGraph& g = g_tiles[d];
			g.interiorsDropped = 0;
			g.bordersSkipped = 0;
			TgSection s;
			memset(&s, 0, sizeof(s));
			s.uid = d;
			s.kind = TGS_EXTERIOR;
			s.gx = d;
			s.nodeCount = 2;
			g.sections.push_back(s);
			for (int i = 0; i < 2; ++i)
			{
				TgNode n;
				memset(&n, 0, sizeof(n));
				n.centre[0] = (float)(d * 4608 + i * 100);
				n.firstArc = i;
				n.arcCount = 1;
				g.nodes.push_back(n);
				TgArc a = { 1 - i, 100.0f };
				g.arcs.push_back(a);
			}
		}
	}

	struct PauseRead { int dir; bool answered; };

	void ReadInsideTake(void* ctx)
	{
		PauseRead* p = (PauseRead*)ctx;
		CgView v;
		p->answered = CgRead(p->dir, &v);
	}

	DWORD WINAPI BaseWriter(void*)
	{
		for (LONG k = 0; k < PUBLISHES; ++k)
		{
			int d = (int)(k % ENTRIES);
			CgBlock* b = CgBlockFromTile(g_tiles[d], 0, CG_BASE, 0);
			if (b && CgPublishBase(d, b) == CGP_OK)
				InterlockedIncrement(&g_basePublished);
			if ((k & 255) == 0) SwitchToThread();
		}
		InterlockedIncrement(&g_buildersDone);
		return 0;
	}

	DWORD WINAPI OverWriter(void*)
	{
		for (LONG k = 0; k < PUBLISHES; ++k)
		{
			int d = (int)((k * 7) % ENTRIES);
			CgBlock* b = CgBlockFromTile(g_tiles[d], 0, CG_SAVE, CgStoreGen());
			if (b && CgPublishOver(d, b) != CGP_NO_SLOT)
				InterlockedIncrement(&g_overPublished);
			if ((k & 255) == 0) SwitchToThread();
		}
		InterlockedIncrement(&g_buildersDone);
		return 0;
	}

	DWORD WINAPI LivePoster(void*)
	{
		LONG k = 0;
		while (g_buildersDone < 2)
		{
			CgBlock* buf = CgLiveAcquire();
			if (!buf)
			{
				SwitchToThread();
				continue;
			}
			int d = (int)(k++ % ENTRIES);
			buf->uid = d;
			buf->nodeCount = 1;
			buf->arcCount = 0;
			buf->borderCount = 0;
			memset(&buf->nodes[0], 0, sizeof(CgNode));
			buf->nodes[0].centre[0] = (float)(d * 4608);
			CgLivePost(d, buf);
			InterlockedIncrement(&g_posts);
			SwitchToThread();
		}
		return 0;
	}

	void ReadPass()
	{
		for (int d = 0; d < ENTRIES; ++d)
		{
			CgView v;
			if (!CgRead(d, &v))
				continue;
			g_reads++;
			if (v.block->uid != d && g_torn++ == 0)
			{
				g_tornEntry = d;
				g_tornUid = v.block->uid;
			}
		}
	}
}
using namespace coarse_graph_injection_detail;

int main()
{
	if (!CgStoreCreate())
	{
		std::printf("store race: setup failed (CgStoreCreate)\n");
		return 1;
	}
	TileGraph tiles[ENTRIES];
	g_tiles = tiles;
	MakeTiles();

	// The paused writer: a reader inside CgPublishOver's take must be refused.
	CgPublishBase(3, CgBlockFromTile(g_tiles[3], 0, CG_BASE, 0));
	PauseRead p = { 3, false };
	CgTestPauseAfterTake(ReadInsideTake, &p);
	CgPublishOver(3, CgBlockFromTile(g_tiles[3], 0, CG_SAVE, CgStoreGen()));
	CgTestPauseAfterTake(NULL, NULL);
	if (p.answered)
	{
		std::printf("store race: HALF-PUBLISHED accepted\n");
		return 1;
	}

	HANDLE threads[3];
	threads[0] = CreateThread(NULL, 0, BaseWriter, NULL, 0, NULL);
	threads[1] = CreateThread(NULL, 0, OverWriter, NULL, 0, NULL);
	threads[2] = CreateThread(NULL, 0, LivePoster, NULL, 0, NULL);
	if (!threads[0] || !threads[1] || !threads[2])
	{
		std::printf("store race: setup failed (CreateThread)\n");
		return 1;
	}
	while (WaitForMultipleObjects(3, threads, TRUE, 0) == WAIT_TIMEOUT)
	{
		ReadPass();
		CgPromoteLive(ENTRIES);
		CgDrainRetired();
		SwitchToThread();
	}
	for (int i = 0; i < 3; ++i)
		CloseHandle(threads[i]);
	ReadPass();
	CgPromoteLive(CG_LIVE_RECORDS);
	CgDrainHandOff();
	CgDrainRetired();

	CgStats s;
	CgStatsGet(&s);
	if (g_torn != 0)
	{
		std::printf("store race: TORN %ld view(s); first: entry %d read uid %d (%ld reads)\n",
		            g_torn, g_tornEntry, g_tornUid, g_reads);
		return 1;
	}
	if (g_basePublished != PUBLISHES || g_overPublished != PUBLISHES || g_reads == 0 || s.promoted == 0)
	{
		std::printf("store race: incomplete run (%ld reads, %ld base, %ld over, %ld promoted)\n",
		            g_reads, g_basePublished, g_overPublished, s.promoted);
		return 1;
	}
	CgStoreDestroy();
	std::printf("store race: %ld reads, 0 torn, %ld busy, %ld published, %ld promoted\n",
	            g_reads, s.busy, s.publishes, s.promoted);
	return 0;
}
