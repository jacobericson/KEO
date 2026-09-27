// CallSiteProbe - see CallSiteProbe.h.

#include "CallSiteProbe.h"

#include <stdio.h>
#include <string.h>

namespace CallSiteProbe
{
namespace callsiteprobe_detail
{
	typedef U64 (*FnInt)(U64, U64, U64, U64);
	typedef U64 (*FnFloat)(U64, float, U64, U64);

	const size_t PAGE_SIZE  = 0x1000;
	const size_t SLOT_SIZE  = 16;
	// Keep the stub page inside rel32 reach of every byte of the module.
	const unsigned long long REACH = 0x7FF00000ULL;

	void*          g_orig[MAX_PROBES];   // indirect sites: the pointer slot
	size_t         g_vslot[MAX_PROBES];
	int            g_tag[MAX_PROBES];
	void*          g_wrapInt[MAX_PROBES];
	void*          g_wrapFloat[MAX_PROBES];
	void*          g_wrapVirt[MAX_PROBES];
	void*          g_wrapInd[MAX_PROBES];
	EnterFn        g_onEnter = NULL;
	ExitFn         g_onExit  = NULL;
	unsigned char* g_page    = NULL;
	int            g_used    = 0;

	// One wrapper per probe id. The body holds no objects, so C++ exceptions
	// thrown by the callee unwind straight through it.
	template<int ID>
	U64 ProbeInt(U64 a, U64 b, U64 c, U64 d)
	{
		EnterFn onEnter = g_onEnter;
		if (onEnter) onEnter(ID, a, b, c, d);
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		U64 r = ((FnInt)g_orig[ID])(a, b, c, d);
		QueryPerformanceCounter(&t1);
		ExitFn onExit = g_onExit;
		if (onExit) onExit(ID, r, t0.QuadPart, t1.QuadPart);
		return r;
	}

	template<int ID>
	U64 ProbeFloat(U64 a, float b, U64 c, U64 d)
	{
		EnterFn onEnter = g_onEnter;
		if (onEnter) onEnter(ID, a, 0, c, d);
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		U64 r = ((FnFloat)g_orig[ID])(a, b, c, d);
		QueryPerformanceCounter(&t1);
		ExitFn onExit = g_onExit;
		if (onExit) onExit(ID, r, t0.QuadPart, t1.QuadPart);
		return r;
	}

	// Virtual site: the patched instruction was `call [rax+vslot]` right after
	// `mov rax,[rcx]`, so dispatch through the same slot of *rcx.
	template<int ID>
	U64 ProbeVirt(U64 a, U64 b, U64 c, U64 d)
	{
		EnterFn onEnter = g_onEnter;
		if (onEnter) onEnter(ID, a, b, c, d);
		FnInt fn = *(FnInt*)(*(const U64*)a + g_vslot[ID]);
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		U64 r = fn(a, b, c, d);
		QueryPerformanceCounter(&t1);
		ExitFn onExit = g_onExit;
		if (onExit) onExit(ID, r, t0.QuadPart, t1.QuadPart);
		return r;
	}

	// Indirect site: the patched instruction was `call [rip+disp32]`, so call
	// through the same pointer slot, read now rather than at install time.
	template<int ID>
	U64 ProbeInd(U64 a, U64 b, U64 c, U64 d)
	{
		EnterFn onEnter = g_onEnter;
		if (onEnter) onEnter(ID, a, b, c, d);
		FnInt fn = *(FnInt volatile*)g_orig[ID];
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		U64 r = fn(a, b, c, d);
		QueryPerformanceCounter(&t1);
		ExitFn onExit = g_onExit;
		if (onExit) onExit(ID, r, t0.QuadPart, t1.QuadPart);
		return r;
	}

	// VS2010 has no variadic templates: fill the wrapper tables recursively.
	template<int N>
	struct FillTables
	{
		static void Run()
		{
			// VS2010 needs the typed pointer first to pick the instantiation.
			FnInt   fi = &ProbeInt<N - 1>;
			FnFloat ff = &ProbeFloat<N - 1>;
			FnInt   fv = &ProbeVirt<N - 1>;
			FnInt   fd = &ProbeInd<N - 1>;
			g_wrapInt[N - 1]   = (void*)fi;
			g_wrapFloat[N - 1] = (void*)ff;
			g_wrapVirt[N - 1]  = (void*)fv;
			g_wrapInd[N - 1]   = (void*)fd;
			FillTables<N - 1>::Run();
		}
	};

	template<>
	struct FillTables<0>
	{
		static void Run() {}
	};

	bool ImageRange(HMODULE module, uintptr_t* base, uintptr_t* end)
	{
		uintptr_t b = (uintptr_t)module;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		*base = b;
		*end  = b + nt->OptionalHeader.SizeOfImage;
		return true;
	}

	void* TryAllocAt(uintptr_t addr)
	{
		return VirtualAlloc((void*)addr, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	}

	// A free page within REACH of [imageBase, imageEnd): first below the image,
	// then above it.
	unsigned char* AllocNear(uintptr_t imageBase, uintptr_t imageEnd)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;

		uintptr_t minAddr = (imageEnd > REACH + gran) ? (uintptr_t)(imageEnd - REACH) : gran;
		uintptr_t maxAddr = imageBase + (uintptr_t)REACH;

		// Downward.
		if (imageBase > gran)
		{
			uintptr_t a = (imageBase - gran) & ~(gran - 1);
			while (a >= minAddr)
			{
				MEMORY_BASIC_INFORMATION mbi;
				if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == 0)
					break;
				if (mbi.State == MEM_FREE)
				{
					void* p = TryAllocAt(a);
					if (p) return (unsigned char*)p;
					if (a < minAddr + gran) break;
					a -= gran;
					continue;
				}
				uintptr_t ab = (uintptr_t)mbi.AllocationBase;
				if (ab == 0 || ab > a) ab = a;
				if (ab < minAddr + gran) break;
				a = (ab - gran) & ~(gran - 1);
			}
		}

		// Upward.
		{
			uintptr_t a = (imageEnd + gran - 1) & ~(gran - 1);
			while (a + PAGE_SIZE <= maxAddr)
			{
				MEMORY_BASIC_INFORMATION mbi;
				if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == 0)
					break;
				if (mbi.State == MEM_FREE)
				{
					void* p = TryAllocAt(a);
					if (p) return (unsigned char*)p;
					a += gran;
					continue;
				}
				uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				next = (next + gran - 1) & ~(gran - 1);
				if (next <= a) next = a + gran;
				a = next;
			}
		}
		return NULL;
	}

	void SetStatus(Site& s, const char* text)
	{
		_snprintf_s(s.status, sizeof(s.status), _TRUNCATE, "%s", text);
	}
}
using namespace callsiteprobe_detail;

void SetCallbacks(EnterFn onEnter, ExitFn onExit)
{
	g_onEnter = onEnter;
	g_onExit  = onExit;
}

const void* StubPage()
{
	return g_page;
}

int TagOf(int id)
{
	if (id < 0 || id >= g_used)
		return -1;
	return g_tag[id];
}

int Install(HMODULE module, Site* sites, int count)
{
	static bool tablesFilled = false;
	if (!tablesFilled)
	{
		FillTables<MAX_PROBES>::Run();
		tablesFilled = true;
	}

	for (int i = 0; i < count; ++i)
	{
		sites[i].id   = -1;
		sites[i].orig = NULL;
		SetStatus(sites[i], "pending");
	}

	uintptr_t base = 0, end = 0;
	if (!ImageRange(module, &base, &end))
	{
		for (int i = 0; i < count; ++i)
			SetStatus(sites[i], "SKIP bad module header");
		return 0;
	}

	// Phase 1: verify each site and reserve a probe id.
	int verified = 0;
	for (int i = 0; i < count; ++i)
	{
		Site& s = sites[i];
		bool virt = s.vslot != 0;
		bool ind  = s.indirect != 0;
		if (s.siteRva < 3 || base + s.siteRva + 6 > end || (virt && ind))
		{
			SetStatus(s, (virt && ind) ? "SKIP both virtual and indirect" : "SKIP outside image");
			continue;
		}
		const unsigned char* p = (const unsigned char*)(base + s.siteRva);
		uintptr_t target = 0;
		if (ind)
		{
			int disp = *(const int*)(p + 2);
			target = (uintptr_t)(p + 6) + (intptr_t)disp;
			if (p[0] != 0xFF || p[1] != 0x15 || target != base + s.targetRva ||
			    target < base || target + 8 > end)
			{
				_snprintf_s(s.status, sizeof(s.status), _TRUNCATE,
				            "SKIP bytes=%02X %02X slot=0x%llX want FF 15 slot=0x%llX",
				            (unsigned)p[0], (unsigned)p[1],
				            (unsigned long long)(target - base), (unsigned long long)s.targetRva);
				continue;
			}
		}
		else if (virt)
		{
			unsigned disp = *(const unsigned*)(p + 2);
			bool loadOk = s.looseVirtual ||
			              (p[-3] == 0x48 && p[-2] == 0x8B && p[-1] == 0x01);
			if (!loadOk || p[0] != 0xFF || p[1] != 0x90 ||
			    disp != (unsigned)s.vslot)
			{
				_snprintf_s(s.status, sizeof(s.status), _TRUNCATE,
				            "SKIP bytes=%02X %02X %02X %02X %02X disp=0x%X want %sFF 90 disp=0x%X",
				            (unsigned)p[-3], (unsigned)p[-2], (unsigned)p[-1], (unsigned)p[0], (unsigned)p[1],
				            disp, s.looseVirtual ? "" : "48 8B 01 ", (unsigned)s.vslot);
				continue;
			}
		}
		else
		{
			if (p[0] != 0xE8)
			{
				_snprintf_s(s.status, sizeof(s.status), _TRUNCATE,
				            "SKIP byte=0x%02X want 0xE8", (unsigned)p[0]);
				continue;
			}
			int rel = *(const int*)(p + 1);
			target = (uintptr_t)(p + 5) + (intptr_t)rel;
			if (target != base + s.targetRva)
			{
				_snprintf_s(s.status, sizeof(s.status), _TRUNCATE,
				            "SKIP target=0x%llX want 0x%llX",
				            (unsigned long long)(target - base),
				            (unsigned long long)s.targetRva);
				continue;
			}
		}
		if (g_used >= MAX_PROBES)
		{
			SetStatus(s, "SKIP no probe id left");
			continue;
		}
		s.id   = g_used++;
		s.orig = (void*)target;
		g_orig[s.id]  = s.orig;
		g_vslot[s.id] = virt ? s.vslot : 0;
		g_tag[s.id]   = s.tag;
		++verified;
	}
	if (verified == 0)
		return 0;

	// Phase 2: stub page and stubs.
	if (!g_page)
		g_page = AllocNear(base, end);
	if (!g_page)
	{
		for (int i = 0; i < count; ++i)
		{
			if (sites[i].id >= 0)
			{
				sites[i].id = -1;
				SetStatus(sites[i], "SKIP no stub page within reach");
			}
		}
		return 0;
	}

	DWORD oldProt = 0;
	VirtualProtect(g_page, PAGE_SIZE, PAGE_READWRITE, &oldProt);
	for (int i = 0; i < count; ++i)
	{
		Site& s = sites[i];
		if (s.id < 0)
			continue;
		unsigned char* slot = g_page + SLOT_SIZE * (size_t)s.id;
		U64 wrapper = (U64)(s.vslot != 0 ? g_wrapVirt[s.id]
		                  : s.indirect != 0 ? g_wrapInd[s.id]
		                  : (s.shape == SHAPE_FLOAT ? g_wrapFloat[s.id] : g_wrapInt[s.id]));
		slot[0] = 0xFF;
		slot[1] = 0x25;
		*(unsigned int*)(slot + 2) = 0;
		memcpy(slot + 6, &wrapper, sizeof(wrapper));
		slot[14] = 0xCC;
		slot[15] = 0xCC;
	}
	VirtualProtect(g_page, PAGE_SIZE, PAGE_EXECUTE_READ, &oldProt);
	FlushInstructionCache(GetCurrentProcess(), g_page, PAGE_SIZE);

	// Phase 3: point each call at its stub, then read it back.
	int installed = 0;
	for (int i = 0; i < count; ++i)
	{
		Site& s = sites[i];
		if (s.id < 0)
			continue;
		unsigned char* p = (unsigned char*)(base + s.siteRva);
		uintptr_t slot = (uintptr_t)(g_page + SLOT_SIZE * (size_t)s.id);
		long long newRel = (long long)slot - (long long)(uintptr_t)(p + 5);
		if (newRel < -2147483647LL - 1 || newRel > 2147483647LL)
		{
			SetStatus(s, "SKIP stub out of rel32 range");
			s.id = -1;
			continue;
		}
		// Virtual and indirect sites are both 6-byte `FF xx disp32` calls.
		bool virt = s.vslot != 0 || s.indirect != 0;
		DWORD prot = 0;
		if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &prot))
		{
			SetStatus(s, "SKIP VirtualProtect failed");
			s.id = -1;
			continue;
		}
		*(int*)(p + 1) = (int)newRel;
		if (virt)
		{
			p[0] = 0xE8;   // call rel32 ...
			p[5] = 0x90;   // ... then a nop over the sixth byte of `FF xx disp32`
		}
		DWORD dummy = 0;
		VirtualProtect(p, 6, prot, &dummy);
		FlushInstructionCache(GetCurrentProcess(), p, 6);

		int check = *(const int*)(p + 1);
		if (p[0] != 0xE8 || (virt && p[5] != 0x90) ||
		    (uintptr_t)(p + 5) + (intptr_t)check != slot)
		{
			SetStatus(s, "SKIP read-back mismatch");
			s.id = -1;
			continue;
		}
		SetStatus(s, "ok");
		++installed;
	}
	return installed;
}

} // namespace CallSiteProbe
