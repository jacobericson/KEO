// audit_sync_split.h - The Ogre barrier probes' pure rules: which side of a fork a main-thread
// Barrier::sync call is, whether it will block, the request bucket, the per-worker reduction, the
// foliage lock's kind, and the one-exchange rewrite of a call a worker may be running. No Windows
// or game header, so the host tests build it alone.

#ifndef KENSHI_FRAME_AUDIT_SYNC_SPLIT_H
#define KENSHI_FRAME_AUDIT_SYNC_SPLIT_H

namespace syncsplit
{

// The barrier's index flips at every completed sync and the workers park at the top one, so the
// main thread's fire reads 0 and its wait 1; anything else is counted, never classed.
enum SyncKind { SK_FIRE, SK_WAIT, SK_ODD };

inline int KindOf(int index)
{
	return index == 0 ? SK_FIRE : (index == 1 ? SK_WAIT : SK_ODD);
}

// The caller will block unless it is the last of the barrier's threads to arrive.
inline bool WouldBlock(int arrived, int threads)
{
	return threads > 0 && arrived < threads - 1;
}

// Ogre's worker requests 0..12 keep their number; request 3 on the light list (the second bounds
// fork) is bucket 13; anything else (another barrier, a bad read) is bucket 14.
const int REQUEST_BUCKETS     = 15;
const int BUCKET_LIGHT_BOUNDS = 13;
const int BUCKET_OTHER        = 14;

inline int RequestBucket(int request, bool lightList)
{
	if (request == 3 && lightList)
		return BUCKET_LIGHT_BOUNDS;
	return request >= 0 && request <= 12 ? request : BUCKET_OTHER;
}

inline long long TicksToUs(long long ticks, long long freq)
{
	return freq > 0 ? ticks * 1000000 / freq : 0;
}

// One worker slot's totals for a frame.
struct WorkerFrame
{
	long long sum, max;
	int       n;
};

struct Reduced
{
	long long sum, max;
	int       n;
	double    mean;   // sum / n over the events, 0 with none
};

inline Reduced Reduce(const WorkerFrame* w, int count)
{
	Reduced r;
	r.sum = 0;
	r.max = 0;
	r.n = 0;
	for (int i = 0; i < count; ++i)
	{
		r.sum += w[i].sum;
		r.n += w[i].n;
		if (w[i].max > r.max)
			r.max = w[i].max;
	}
	r.mean = r.n > 0 ? (double)r.sum / (double)r.n : 0.0;
	return r;
}

// HardwareBuffer::LockOptions: 1 discard (a destination), 2 read-only (a source).
inline bool IsReadOnlyLock(int option)
{
	return option == 2;
}

// A call another thread may run while it is rewritten changes in one aligned 8-byte exchange, so
// its bytes [site, site + len) must lie inside one aligned qword.
inline bool InOneQword(unsigned long long site, int len)
{
	return len > 0 && len <= 8 && (site & ~7ULL) == ((site + (unsigned long long)len - 1) & ~7ULL);
}

// The little-endian qword `before` with the 6-byte call at byte `off` rewritten as E8 rel32 + 90;
// the bytes outside the call are kept. `before` unchanged when the call does not fit.
inline unsigned long long CallQword(unsigned long long before, int off, int rel32)
{
	if (off < 0 || off > 2)
		return before;
	unsigned long long call = 0xE8ULL | ((unsigned long long)(unsigned int)rel32 << 8) | (0x90ULL << 40);
	unsigned long long mask = 0xFFFFFFFFFFFFULL << (8 * off);
	return (before & ~mask) | (call << (8 * off));
}

} // namespace syncsplit

#endif
