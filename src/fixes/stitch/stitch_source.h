#ifndef KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_H
#define KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_H

// Who wrote the stale connection records the un-stitch bounds guard drops.
//
// The stitch writes a section pair's connection records into both graphs'
// streaming sets, each record carrying an opposite node index drawn from the
// other graph. The un-stitch reads them back at teardown against whatever
// instance the streaming collection then holds for the opposite section. The
// write runs under the generator's build lock, the read under changeMutex, and
// nothing ties the index to the graph it was drawn from.
//
// The write side is a pass-through on the stitch that records, after the
// original returns, what each graph's set now holds: the index range, the
// opposite graph and its node count, and which caller reached it. No lock, no
// allocation and no logging there -- interlocked counters and a lock-free
// table only. The read side is called by the guard for every set it drops a
// record from, looks the write up and classifies the drop in one line.

// Every build; the counters exist whenever the prologue verifies.
void InstallStitchSource(int* installed, int*);

// Streaming-collection insert observer, called after the original has run.
void StitchSourceOnAdd(void* collection, __int64 sectionData, __int64 graphInstance);
// The insert hook is installed this session: its fields print as numbers
// rather than "?".
void StitchSourceNoteAddObserver();

// The guard is about to take its replacement walk for this graph instance;
// callNo is the guard's un-stitch call number. A teardown enters the un-stitch
// twice on one instance, back to back; the second entry is counted as a
// repeat and not classified again.
void StitchSourceBeginFired(const void* graphInstance, long callNo);

// One set lost records in the replacement walk. mapSize is the opposite
// instance's node-map size when the first record was dropped.
void StitchSourceOnSetDrops(const void* graphInstance, int ownUid, int oppUid, int setConnCount,
                            const void* oppGraphInstance, int mapSize,
                            int minDropIdx, int maxDropIdx, int drops);

// Main thread, every frame: the heartbeat, independent of the guard's key.
void StitchSourceTick(double now);

#endif // KENSHI_ZONE_OPT_FIXES_STITCH_SOURCE_H
