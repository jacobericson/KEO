#include "navmesh/generation/misspar_math.h"
#include <algorithm>
#include <sstream>

static const unsigned __int64 FNV_BASIS = 0xCBF29CE484222325ULL;
static const unsigned __int64 FNV_PRIME = 0x100000001B3ULL;

static unsigned __int64 Mix(unsigned __int64 h, const unsigned char* p, int n)
{
	for (int i = 0; i < n; ++i) { h ^= p[i]; h *= FNV_PRIME; }
	return h;
}

unsigned __int64 MissParFnv64(const MissParSpan* spans, int n)
{
	unsigned __int64 h = FNV_BASIS;
	for (int i = 0; i < n; ++i)
	{
		unsigned int len = (unsigned int)(spans[i].data ? spans[i].bytes : 0);
		unsigned char le[4] = { (unsigned char)len, (unsigned char)(len >> 8),
		                        (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
		h = Mix(h, le, 4);
		if (len)
			h = Mix(h, (const unsigned char*)spans[i].data, (int)len);
	}
	return h;
}

static bool ByStart(const MissParInterval& a, const MissParInterval& b) { return a.start < b.start; }

__int64 MissParUnion(MissParInterval* v, int n, __int64* sumOut)
{
	__int64 sum = 0, uni = 0;
	if (n > 0)
	{
		std::sort(v, v + n, ByStart);
		__int64 curS = v[0].start, curE = v[0].end;
		for (int i = 0; i < n; ++i)
		{
			sum += v[i].end - v[i].start;
			if (i == 0) continue;
			if (v[i].start > curE) { uni += curE - curS; curS = v[i].start; curE = v[i].end; }
			else if (v[i].end > curE) curE = v[i].end;
		}
		uni += curE - curS;
	}
	if (sumOut) *sumOut = sum;
	return uni;
}

int MissParGenConcurrency(int cfg, int logicalCpus, int cap)
{
	if (cap < 1) cap = 1;
	if (cfg > 0)
		return cfg < cap ? cfg : cap;
	int n = logicalCpus - 3;
	if (n > cap) n = cap;
	return n < 1 ? 1 : n;
}

std::string MissParGenConcurrencyMessage(int cfg, int cap, int logicalCpus, bool splitOff)
{
	std::ostringstream ss;
	ss << "NavMesh gen concurrency: configured=" << cfg << " resolved cap=" << cap;
	if (cfg > 0)
		ss << " (explicit)";
	else
		ss << " (auto from " << logicalCpus << " cpus)";
	if (splitOff)
		ss << " (inert: navmeshMissSplit is off, generation stays serial under processJobCS)";
	return ss.str();
}

int MissParClassifyRelease(const MissParReleaseInputs& in)
{
	if (!in.splitEnabled || !in.slotsReady || !in.keycodesReady || in.stopSeen)
		return MP_REL_NONE;
	if (in.pjDepth != 1 || in.wb == 0)
		return MP_REL_NONE;

	if (in.armKind == MP_ARM_CLONE)
		return in.wb != in.installedWb ? MP_REL_CLONE : MP_REL_NONE;

	if (in.armKind == MP_ARM_SWAP)
	{
		if (!in.bgSplitEnabled || !in.holderIsBg || in.canonicalWb == 0)
			return MP_REL_NONE;
		if (in.wb != in.installedWb || in.wb == in.canonicalWb)
			return MP_REL_NONE;   // no fresh buffer is installed: this is the real one
		if (in.swapOutstanding != 1)
			return MP_REL_NONE;
		return MP_REL_SWAP;
	}
	return MP_REL_NONE;
}

void MissParCanonInit(MissParCanonState& st)
{
	st.cand = 0;
	st.agree = 0;
	st.disabled = false;
}

void MissParCanonObserve(MissParCanonState& st, const void* observed, int needAgree)
{
	if (st.disabled || observed == 0)
		return;
	if (st.cand == observed)
	{
		if (st.agree < needAgree)
			++st.agree;
		return;
	}
	if (st.agree >= needAgree)
	{
		// The proven buffer changed under us: the proof was wrong, or the
		// generator was rebuilt. Either way stop restoring a pointer we can no
		// longer vouch for.
		st.disabled = true;
		return;
	}
	st.cand = observed;
	st.agree = 1;
}

const void* MissParCanonConfirmed(const MissParCanonState& st, int needAgree)
{
	return (!st.disabled && st.agree >= needAgree) ? st.cand : 0;
}
