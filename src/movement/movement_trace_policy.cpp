// movement_trace_policy.cpp - The movement trace's pure rules, the result ring's sequence and the
// trace file's lines. No lock; the result ring's writer and reader are each one thread.
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include "movement/movement_trace_policy.h"

#pragma intrinsic(_ReadWriteBarrier, _InterlockedIncrement)

bool TraceSampleDue(bool haveLast, float lastX, float lastZ, float x, float z)
{
	if (!haveLast)
		return true;
	float dx = x - lastX, dz = z - lastZ;
	return dx * dx + dz * dz >= TRACE_SAMPLE_UNITS * TRACE_SAMPLE_UNITS;
}

void TraceRingPush(TraceRing* r, const TraceSample& s)
{
	r->s[r->head] = s;
	r->head = (r->head + 1) % TRACE_RING;
	if (r->count < TRACE_RING)
		++r->count;
	else
		++r->drops;
}

int TraceRingCopy(const TraceRing& r, TraceSample* out)
{
	int first = (r.head - r.count + TRACE_RING) % TRACE_RING;
	for (int i = 0; i < r.count; ++i)
		out[i] = r.s[(first + i) % TRACE_RING];
	return r.count;
}

TraceResult* TraceResultBegin(TraceResultRing* r)
{
	int k = (int)(r->published % TRACE_RESULT_RING);
	_InterlockedIncrement(&r->slotSeq[k]);   // odd: writing
	_ReadWriteBarrier();
	return &r->slot[k];
}

void TraceResultEnd(TraceResultRing* r)
{
	int k = (int)(r->published % TRACE_RESULT_RING);
	_ReadWriteBarrier();
	_InterlockedIncrement(&r->slotSeq[k]);   // even: whole
	_InterlockedIncrement(&r->published);
}

bool TraceResultTake(TraceResultRing* r, long* taken, TraceResult* out, long* overruns, long* torn)
{
	for (;;)
	{
		long published = r->published;
		if (*taken >= published)
			return false;
		if (published - *taken > TRACE_RESULT_RING)
		{
			*overruns += published - TRACE_RESULT_RING - *taken;
			*taken = published - TRACE_RESULT_RING;
		}
		int k = (int)(*taken % TRACE_RESULT_RING);
		long seq1 = r->slotSeq[k];
		_ReadWriteBarrier();
		if (!(seq1 & 1))
			memcpy(out, &r->slot[k], sizeof(*out));
		_ReadWriteBarrier();
		long seq2 = r->slotSeq[k];
		long after = r->published;
		long mine = (*taken)++;
		if (!(seq1 & 1) && seq1 == seq2 && after - mine <= TRACE_RESULT_RING)
			return true;
		++*torn;
	}
}

int TraceAttribute(const float firstXz[2], const float* lastXz, const int* have, int n)
{
	int best = -1;
	float bestSq = TRACE_ATTRIB_UNITS * TRACE_ATTRIB_UNITS;
	for (int i = 0; lastXz && have && i < n; ++i)
	{
		if (!have[i])
			continue;
		float dx = lastXz[2 * i] - firstXz[0], dz = lastXz[2 * i + 1] - firstXz[1];
		float d2 = dx * dx + dz * dz;
		if (d2 <= bestSq)
		{
			bestSq = d2;
			best = i;
		}
	}
	return best;
}

void TraceHavokToWorld(const float h[3], const float shift[3], float out[3])
{
	for (int k = 0; k < 3; ++k)
		out[k] = (h[k] - shift[k]) * 10.0f;
}

std::string TraceFormatOrder(const TraceOrderLine& o)
{
	char buf[192];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "o order=%d from=(%.1f,%.1f) to=(%.1f,%.1f) members=%d t0=%.3f",
	            o.order, o.fromX, o.fromZ, o.toX, o.toZ, o.members, o.t0);
	return buf;
}

std::string TraceFormatSample(const TraceSample& s)
{
	char buf[192];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
	            "s order=%d member=%d t=%.3f x=%.1f y=%.1f z=%.1f ps=%d cs=%d cell=%d.%d leg=%d",
	            s.order, s.member, s.t, s.x, s.y, s.z, s.pathState, s.characterState, s.cellX, s.cellY, s.leg);
	return buf;
}

std::string TraceFormatResult(const TraceResultLine& p)
{
	char buf[96];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "p order=%d member=%d t=%.3f cut=%d n=%d",
	            p.order, p.member, p.t, p.cut, p.count);
	std::string out = buf;
	for (size_t i = 0; i < p.nodes.size(); ++i)
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, " %u:%.1f,%.1f,%.1f",
		            p.nodes[i].face, p.nodes[i].x, p.nodes[i].y, p.nodes[i].z);
		out += buf;
	}
	return out;
}

int TraceParseLine(const char* line, TraceLine* out)
{
	out->kind = TLK_NONE;
	out->p.nodes.clear();
	if (!line)
		return TLK_NONE;
	if (line[0] == 'o' && line[1] == ' ')
	{
		TraceOrderLine& o = out->o;
		if (sscanf_s(line, "o order=%d from=(%f,%f) to=(%f,%f) members=%d t0=%lf",
		             &o.order, &o.fromX, &o.fromZ, &o.toX, &o.toZ, &o.members, &o.t0) == 7)
			out->kind = TLK_ORDER;
	}
	else if (line[0] == 's' && line[1] == ' ')
	{
		TraceSample& s = out->s;
		if (sscanf_s(line, "s order=%d member=%d t=%lf x=%f y=%f z=%f ps=%d cs=%d cell=%d.%d leg=%d",
		             &s.order, &s.member, &s.t, &s.x, &s.y, &s.z, &s.pathState, &s.characterState,
		             &s.cellX, &s.cellY, &s.leg) == 11)
			out->kind = TLK_SAMPLE;
	}
	else if (line[0] == 'p' && line[1] == ' ')
	{
		TraceResultLine& p = out->p;
		int used = 0;
		if (sscanf_s(line, "p order=%d member=%d t=%lf cut=%d n=%d%n",
		             &p.order, &p.member, &p.t, &p.cut, &p.count, &used) != 5)
			return TLK_NONE;
		const char* q = line + used;
		TraceNode n;
		int step = 0;
		while (sscanf_s(q, " %u:%f,%f,%f%n", &n.face, &n.x, &n.y, &n.z, &step) == 4)
		{
			p.nodes.push_back(n);
			q += step;
		}
		if (*q == '\0')
			out->kind = TLK_RESULT;
	}
	return out->kind;
}
