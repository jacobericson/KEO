// movement_trace_policy.h - The movement trace's pure rules: when a member is sampled, a member's
// sample ring, the path-result ring between the path thread and the main thread, a result's
// attribution to a member, the move out of the shifted Havok frame, and the trace file's four line
// formats with their parser, which the offline harness reads the file with. No game or Windows
// header; the result ring's writer is one thread and its reader another, every other rule any thread.
#ifndef KEO_MOVEMENT_TRACE_POLICY_H
#define KEO_MOVEMENT_TRACE_POLICY_H

#include <string>
#include <vector>

const float  TRACE_SAMPLE_UNITS  = 75.0f;   // a member this far (x-z) from its last sample is sampled
const int    TRACE_RING          = 4096;    // samples one member keeps, the newest: a long order at the spacing
const int    TRACE_MEMBERS       = 64;      // members traced at once
const int    TRACE_RESULT_NODES  = 256;     // nodes a path result keeps; a longer chain is cut
const int    TRACE_RESULT_RING   = 16;      // results in flight from the path thread to the main thread
const float  TRACE_ATTRIB_UNITS  = 50.0f;   // a request's start this near a sample: above 37.5, half the spacing
const double TRACE_ATTRIB_SECONDS = 3.0;     // a result's member is sought among samples this recent, and its newest
const int    TRACE_ORDER_RESULTS = 1024;    // results one order keeps
const double TRACE_LINE_SECONDS  = 30.0;    // the Trace: line's period

enum TraceMode { TRACE_OFF = 0, TRACE_ON };

// Whether a member at (x, z) is sampled: no sample yet, its path state, character state or leg
// changed since its last sample (stateChanged), or TRACE_SAMPLE_UNITS or more (x-z) from its last one.
bool TraceSampleDue(bool haveLast, float lastX, float lastZ, float x, float z, bool stateChanged);

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
	float    start[3];   // the request's start point
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

// The member a result belongs to: of the n members whose rings[i] is set, the one with a sample lying
// nearest startXz (the request's start point) and within TRACE_ATTRIB_UNITS, among its newest sample
// and every sample taken no earlier than resultT - TRACE_ATTRIB_SECONDS (the path latency it walked on
// through); -1 when none.
// Each ring is read newest first, stopping at the first sample older than the window.
int  TraceAttribute(const float startXz[2], double resultT, const TraceRing* const* rings, int n);
// A shifted Havok point in world units: (h - shift) * 10, per axis.
void TraceHavokToWorld(const float h[3], const float shift[3], float out[3]);
// A result's request start point in world units, x and z: the point its member is sought by. A start
// between two of its samples, taken TRACE_SAMPLE_UNITS apart, lies within half that of one of them.
void TraceResultStartXz(const TraceResult& r, float outXz[2]);

// The trace file's lines.
struct TraceOrderLine { int order; float fromX, fromZ, toX, toZ; int members; double t0; };
struct TraceNode { unsigned face; float x, y, z; };
struct TraceResultLine { int order, member; double t; int cut, count; std::vector<TraceNode> nodes; };
struct TraceLaunchLine { unsigned pid; int year, month, day, hour, minute, second; double t; };
// "o order=<n> from=(<x>,<z>) to=(<x>,<z>) members=<n> t0=<t>"
std::string TraceFormatOrder(const TraceOrderLine& o);
// "s order=<n> member=<k> t=<t> x=<x> y=<y> z=<z> ps=<n> cs=<n> cell=<cx>.<cy> leg=<n>"
std::string TraceFormatSample(const TraceSample& s);
// "p order=<n> member=<k> t=<t> cut=<0|1> n=<count>", then " <face>:<x>,<y>,<z>" per node; member -1
// for a result no member took, written under the next closed order
std::string TraceFormatResult(const TraceResultLine& p);
// "l pid=<n> armed=<YYYY-MM-DD HH:MM:SS> t=<t>": one per launch, written when the trace arms
std::string TraceFormatLaunch(const TraceLaunchLine& l);
enum TraceLineKind { TLK_NONE = 0, TLK_ORDER, TLK_SAMPLE, TLK_RESULT, TLK_LAUNCH };
struct TraceLine { int kind; TraceOrderLine o; TraceSample s; TraceResultLine p; TraceLaunchLine l; };
// One line of a trace file into out: its kind, TLK_NONE for a blank or malformed line.
int  TraceParseLine(const char* line, TraceLine* out);

#endif // KEO_MOVEMENT_TRACE_POLICY_H
