// movement_trace_policy.h - The movement trace's pure rules: when a member is sampled, a member's
// sample ring, the path-result ring between the path thread and the main thread, a result's
// attribution to a member, the move out of the shifted Havok frame, and the trace file's three line
// formats with their parser, which the offline harness reads the file with. No game or Windows
// header; the result ring's writer is one thread and its reader another, every other rule any thread.
#ifndef KEO_MOVEMENT_TRACE_POLICY_H
#define KEO_MOVEMENT_TRACE_POLICY_H

#include <string>
#include <vector>

const float  TRACE_SAMPLE_UNITS  = 3.0f;    // a member this far (x-z) from its last sample is sampled
const int    TRACE_RING          = 1024;    // samples one member keeps, the newest
const int    TRACE_MEMBERS       = 64;      // members traced at once
const int    TRACE_RESULT_NODES  = 256;     // nodes a path result keeps; a longer chain is cut
const int    TRACE_RESULT_RING   = 16;      // results in flight from the path thread to the main thread
const float  TRACE_ATTRIB_UNITS  = 50.0f;   // a result's first point this near a member's last sample
const int    TRACE_ORDER_RESULTS = 1024;    // results one order keeps
const double TRACE_LINE_SECONDS  = 30.0;    // the Trace: line's period

enum TraceMode { TRACE_OFF = 0, TRACE_ON };

// Whether a member at (x, z) is sampled: no sample yet, or TRACE_SAMPLE_UNITS or more (x-z) from
// its last one.
bool TraceSampleDue(bool haveLast, float lastX, float lastZ, float x, float z);

struct TraceSample
{
	int    order, member;
	double t;
	float  x, y, z;
	int    pathState, characterState, cellX, cellY, leg;
};
// One member's samples: the newest TRACE_RING kept; drops counts the overwritten ones.
struct TraceRing
{
	int         head, count;
	long        drops;
	TraceSample s[TRACE_RING];
};
void TraceRingPush(TraceRing* r, const TraceSample& s);
// The ring's samples, oldest first, into out (room for TRACE_RING); returns their count.
int  TraceRingCopy(const TraceRing& r, TraceSample* out);

// One path result as the path thread copied it, in the shifted Havok frame.
struct TraceResult
{
	double   t;
	int      count;    // the result's node count
	int      copied;   // the nodes kept: count, at most TRACE_RESULT_NODES
	int      cut;      // 1 when count passes TRACE_RESULT_NODES
	float    shift[3];
	unsigned face[TRACE_RESULT_NODES];
	float    mid[TRACE_RESULT_NODES][3];   // each node's mLeft/mRight midpoint
};
// One writer and one reader: published counts the results ever published; a slot's sequence is odd
// while the writer fills it.
struct TraceResultRing
{
	volatile long published;
	volatile long slotSeq[TRACE_RESULT_RING];
	TraceResult   slot[TRACE_RESULT_RING];
};
// Writer: the slot the next result goes into, its sequence made odd.
TraceResult* TraceResultBegin(TraceResultRing* r);
// Writer: that slot's sequence made even, then the result published.
void         TraceResultEnd(TraceResultRing* r);
// Reader: the next published result after *taken copied into out. A result more than
// TRACE_RESULT_RING behind the writer is skipped and counted in *overruns; a slot caught mid-write, or
// rewritten during the copy, is skipped and counted in *torn. False when nothing is left.
bool         TraceResultTake(TraceResultRing* r, long* taken, TraceResult* out, long* overruns, long* torn);

// The member a result belongs to: of the n members whose have[i] is set, the one whose last sample
// (lastXz[2i], lastXz[2i + 1]) lies nearest firstXz and within TRACE_ATTRIB_UNITS; -1 when none.
int  TraceAttribute(const float firstXz[2], const float* lastXz, const int* have, int n);
// A shifted Havok point in world units: (h - shift) * 10, per axis.
void TraceHavokToWorld(const float h[3], const float shift[3], float out[3]);

// The trace file's lines.
struct TraceOrderLine { int order; float fromX, fromZ, toX, toZ; int members; double t0; };
struct TraceNode { unsigned face; float x, y, z; };
struct TraceResultLine { int order, member; double t; int cut, count; std::vector<TraceNode> nodes; };
// "o order=<n> from=(<x>,<z>) to=(<x>,<z>) members=<n> t0=<t>"
std::string TraceFormatOrder(const TraceOrderLine& o);
// "s order=<n> member=<k> t=<t> x=<x> y=<y> z=<z> ps=<n> cs=<n> cell=<cx>.<cy> leg=<n>"
std::string TraceFormatSample(const TraceSample& s);
// "p order=<n> member=<k> t=<t> cut=<0|1> n=<count>", then " <face>:<x>,<y>,<z>" per node
std::string TraceFormatResult(const TraceResultLine& p);
enum TraceLineKind { TLK_NONE = 0, TLK_ORDER, TLK_SAMPLE, TLK_RESULT };
struct TraceLine { int kind; TraceOrderLine o; TraceSample s; TraceResultLine p; };
// One line of a trace file into out: its kind, TLK_NONE for a blank or malformed line.
int  TraceParseLine(const char* line, TraceLine* out);

#endif // KEO_MOVEMENT_TRACE_POLICY_H
