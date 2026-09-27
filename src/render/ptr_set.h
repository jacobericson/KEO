#pragma once
#include <cstddef>
#include <cstring>

// A fixed-capacity set of pointers for one thread: linear probing with
// backward-shift deletion, so there are no tombstones and a removal leaves
// every other entry reachable. N is a power of two; fill stops at 3/4 of it
// so every probe chain ends at an empty slot. No constructor: a static
// instance starts zeroed, i.e. empty.
template <int N>
struct PtrSet
{
	const void* slots[N];
	int         count;

	static const int MAX_FILL = N / 4 * 3;

	void Clear()
	{
		memset(slots, 0, sizeof(slots));
		count = 0;
	}

	bool Contains(const void* p) const
	{
		return p && slots[Find(p)] == p;
	}

	// False when p is new and the set is full; p is then not added.
	bool Add(const void* p)
	{
		int i = Find(p);
		if (slots[i] == p)
			return true;
		if (count >= MAX_FILL)
			return false;
		slots[i] = p;
		++count;
		return true;
	}

	void Remove(const void* p)
	{
		int i = Find(p);
		if (!p || slots[i] != p)
			return;
		// Pull later members of the chain back over the hole unless their
		// home slot lies cyclically in (hole, j]: they are reachable as is.
		int j = i;
		for (;;)
		{
			j = (j + 1) & (N - 1);
			if (!slots[j])
				break;
			int k = Home(slots[j]);
			bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
			if (stays)
				continue;
			slots[i] = slots[j];
			i = j;
		}
		slots[i] = NULL;
		--count;
	}

private:
	static int Home(const void* p)
	{
		return (int)((unsigned)((size_t)p >> 4) * 2654435761u) & (N - 1);
	}

	// The slot holding p, or the empty slot that ends its chain.
	int Find(const void* p) const
	{
		int i = Home(p);
		while (slots[i] && slots[i] != p)
			i = (i + 1) & (N - 1);
		return i;
	}
};
