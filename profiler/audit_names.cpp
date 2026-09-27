// audit_names.cpp - Names and module bounds for render attribution.
// Main thread publishes names; the reporter reads published entries. No mod locks.

#include "audit_detail.h"
#include <stdio.h>

namespace kenshiframeaudit_detail {
// =========================================================================
// Render attribution tables. The main thread appends entries (publishing the
// new count last) and bumps the counters; the reporter reads published
// entries and snapshots the counters. Entry 0 of every table is a catch-all.
// =========================================================================

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
const int CLASS_HASH  = 1024;   // power of two, well above MAX_CLASSES
const int MESH_HASH   = 16384;  // power of two, well above MAX_MESHES

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

NameEntry     g_camE[MAX_NAMED], g_vpE[MAX_NAMED], g_clsE[MAX_CLASSES], g_meshE[MAX_MESHES];
volatile LONG g_camN = 0, g_vpN = 0, g_clsN = 0, g_meshN = 0;

const void*    g_clsKey[CLASS_HASH];
unsigned short g_clsVal[CLASS_HASH];
const void*    g_meshKey[MESH_HASH];
unsigned short g_meshVal[MESH_HASH];

// [0] = main-view draws, [1] = shadow-caster draws (render stage 1).
volatile LONG   g_clsDraws[2][MAX_CLASSES];
volatile LONG   g_clsInst[2][MAX_CLASSES];
volatile LONG64 g_clsTsc[2][MAX_CLASSES];      // renderSingleObject time, TSC ticks
volatile LONG   g_meshDraws[2][MAX_MESHES];
volatile LONG   g_meshBatch[2][MAX_MESHES];    // of those, through an InstanceBatch
volatile LONG   g_meshInst[2][MAX_MESHES];     // numberOfInstances summed
volatile LONG   g_drawFrames = 0;              // frames closed with RenderDetail on
volatile double g_tscPerMs   = 0.0;            // TSC ticks per ms, calibrated by the reporter

// CharBody::update time on the AI thread, by the class of the task it runs
// (QPC ticks), merged by the main thread after each join.
volatile LONG64 g_taskTicks[MAX_CLASSES];
volatile LONG   g_taskCalls[MAX_CLASSES];
volatile LONG64 g_taskNoneTicks = 0;           // characters with no current task
volatile LONG   g_taskNoneCalls = 0;
volatile LONG   g_aiFrames      = 0;           // AI runs collected

// Particle effects by ParticleUniverse template name. Entry 0 collects
// _update calls the census could not name. The census (main thread) adds the
// per-frame states; the _update detour adds time and calls from any thread.
// [0] = system on screen (drawn within the last frame), [1] = off screen.
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
NameEntry       g_fxE[MAX_FX_TPL];
volatile LONG   g_fxN = 0;
volatile LONG64 g_fxTsc[2][MAX_FX_TPL];        // _update time, TSC ticks
volatile LONG   g_fxCalls[2][MAX_FX_TPL];
volatile LONG64 g_fxAlive[MAX_FX_TPL];         // census sums (divide by census frames)
volatile LONG64 g_fxVisSum[MAX_FX_TPL];
volatile LONG64 g_fxStopSum[MAX_FX_TPL];
volatile LONG64 g_fxPartSum[MAX_FX_TPL];
volatile LONG   g_fxNew[MAX_FX_TPL];           // entered the active list
volatile LONG   g_fxDel[MAX_FX_TPL];           // left it
volatile LONG   g_fxFrames    = 0;             // censuses taken
volatile LONG64 g_fxEffSum    = 0;             // active effects, with or without particles
volatile LONG64 g_fxNvtoSum   = 0;             // systems with nonvisible_update_timeout set
volatile LONG64 g_fxMainTsc   = 0;             // _update on the main thread (zone effects), main only
volatile LONG   g_fxMainCalls = 0;
volatile double g_fxRunMs     = 0.0;           // unpaused frame time over the censuses
volatile LONG   g_fxOverflow  = 0;             // a census had more systems than its map holds

void InitNameTables()
{
	memset(g_camE, 0, sizeof(g_camE));
	memset(g_vpE, 0, sizeof(g_vpE));
	memset(g_clsE, 0, sizeof(g_clsE));
	memset(g_meshE, 0, sizeof(g_meshE));
	memset(g_fxE, 0, sizeof(g_fxE));
	strcpy_s(g_camE[0].name, "(unknown)");
	strcpy_s(g_vpE[0].name, "(unknown)");
	strcpy_s(g_clsE[0].name, "(unresolved)");
	strcpy_s(g_meshE[0].name, "(unnamed or table full)");
	strcpy_s(g_fxE[0].name, "(unmapped)");
	g_camN = g_vpN = g_clsN = g_meshN = g_fxN = 1;
}


} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {
// ---- Naming what is rendered (main thread; only on first sight of a key) --

// A VS2010 std::string (Ogre's and this DLL's layout): 16-byte buffer or
// heap pointer, then size and capacity. Keeps the tail when it doesn't fit,
// since the end of a resource path is the informative part.
void CopyOgreString(const void* s, char* out, size_t cap)
{
	out[0] = 0;
	if (!s || cap < 2)
		return;
	size_t size = *(const size_t*)(KLIB_MEMBER(5, s, StdString_size, 16));
	size_t res  = *(const size_t*)(KLIB_MEMBER(5, s, StdString_capacity, 24));
	if (size > res || res > (1u << 20))
		return;
	const char* p = res >= 16 ? *(const char* const*)KLIB_MEMBER(5, s, StdString_pointer, 0) : (const char*)KLIB_MEMBER(5, s, StdString_buffer, 0);
	if ((uintptr_t)p < 0x10000 || (uintptr_t)p >= 0x00007FFFFFFFFFFFULL)
		return;   // not a string: never follow a pointer that can't be a heap buffer
	size_t start = size >= cap ? size - (cap - 1) : 0;
	memcpy(out, p + start, size - start);
	out[size - start] = 0;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;


namespace audit {

// VirtualQuery rather than GetModuleHandleEx: it takes no loader lock (this
// runs on the render thread).
bool ModuleRange(const void* p, uintptr_t* base, uintptr_t* end)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.Type != MEM_IMAGE || !mbi.AllocationBase)
		return false;
	uintptr_t b = (uintptr_t)mbi.AllocationBase;
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

} // audit


namespace kenshiframeaudit_detail {

inline bool InImage(uintptr_t a, size_t len, uintptr_t base, uintptr_t end)
{
	return a >= base && a + len >= a && a + len <= end;
}

// NUL-terminated string inside the image, copied only if it ends in bounds.
bool CopyImageString(uintptr_t a, uintptr_t base, uintptr_t end, char* out, size_t cap)
{
	for (size_t i = 0; i + 1 < cap; ++i)
	{
		if (!InImage(a + i, 1, base, end))
			return false;
		char ch = *(const char*)(a + i);
		out[i] = ch;
		if (ch == 0)
			return true;
	}
	out[cap - 1] = 0;
	return true;
}

// ".?AVSubEntity@Ogre@@" -> "Ogre::SubEntity"; templates stay raw.
void DemangleTypeName(const char* raw, char* out, size_t cap)
{
	const char* s = raw;
	if (strncmp(s, ".?AV", 4) == 0 || strncmp(s, ".?AU", 4) == 0)
		s += 4;
	std::string body(s);
	if (body.size() >= 2 && body.compare(body.size() - 2, 2, "@@") == 0)
		body.erase(body.size() - 2);
	std::string result;
	if (body.find('?') != std::string::npos)
		result = body;
	else
	{
		size_t endPos = body.size();
		while (true)
		{
			size_t at = body.rfind('@', endPos == 0 ? 0 : endPos - 1);
			if (at == std::string::npos || endPos == 0)
			{
				result += body.substr(0, endPos);
				break;
			}
			result += body.substr(at + 1, endPos - at - 1) + "::";
			endPos = at;
		}
	}
	strncpy_s(out, cap, result.c_str(), _TRUNCATE);
}

// MSVC x64 RTTI of a vtable: class name, the offset of this vtable's
// subobject inside the complete object, and whether the class derives from
// Ogre::InstanceBatch. Every read is checked against the module's image.
bool ResolveClass(const void* vtable, NameEntry& e)
{
	uintptr_t base = 0, end = 0;
	if (!ModuleRange(vtable, &base, &end))
		return false;
	uintptr_t vt = (uintptr_t)vtable;
	if (!InImage(vt - 8, 8, base, end))
		return false;
	uintptr_t col = *(const uintptr_t*)(vt - 8);
	if (!InImage(col, 24, base, end))
		return false;
	const DWORD* c = (const DWORD*)col;   // signature, offset, cdOffset, type, hierarchy, self
	if (c[0] != 1 || (uintptr_t)c[5] != col - base)
		return false;
	uintptr_t td = base + c[3];
	char raw[160];
	if (!InImage(td, 16, base, end) || !CopyImageString(td + 16, base, end, raw, sizeof(raw)))
		return false;
	DemangleTypeName(raw, e.name, NAME_LEN);
	e.objOffset = (int)c[1];
	e.kind      = KIND_OTHER;
	if (strcmp(raw, ".?AVSubEntity@Ogre@@") == 0)
	{
		e.kind = KIND_SUBENTITY;
		return true;
	}
	uintptr_t chd = base + c[4];
	if (!InImage(chd, 16, base, end))
		return true;
	const DWORD* h = (const DWORD*)chd;   // signature, attributes, numBaseClasses, baseClassArray
	DWORD n = h[2];
	uintptr_t arr = base + h[3];
	if (n > 64 || !InImage(arr, 4 * (size_t)n, base, end))
		return true;
	for (DWORD i = 0; i < n; ++i)
	{
		uintptr_t bcd = base + ((const DWORD*)arr)[i];
		if (!InImage(bcd, 4, base, end))
			break;
		uintptr_t btd = base + *(const DWORD*)bcd;
		char bn[64];
		if (InImage(btd, 16, base, end) && CopyImageString(btd + 16, base, end, bn, sizeof(bn)) &&
		    strcmp(bn, ".?AVInstanceBatch@Ogre@@") == 0)
		{
			e.kind = KIND_BATCH;
			break;
		}
	}
	return true;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

// Keys cached in each hash (failed lookups included). Inserting stops at half
// the slots, so a probe sequence always meets an empty slot quickly.
int g_clsKeys  = 0;
int g_meshKeys = 0;
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

// Class id of a Renderable's vtable; 0 when RTTI can't be read or the table is full.
int ClassIdOf(const void* vtable)
{
	unsigned h = HashPtr(vtable, CLASS_HASH - 1);
	for (int probe = 0; probe < MAX_PROBES_PER_LOOKUP; ++probe)
	{
		const void* k = g_clsKey[h];
		if (k == vtable)
			return g_clsVal[h];
		if (!k)
		{
			if (g_clsKeys >= CLASS_HASH / 2)
				return 0;
			int id = 0;
			LONG n = g_clsN;
			if (n < MAX_CLASSES)
			{
				NameEntry& e = g_clsE[n];
				memset(&e, 0, sizeof(e));
				e.key = vtable;
				if (ResolveClass(vtable, e))
				{
					id = (int)n;
					_WriteBarrier();
					InterlockedExchange(&g_clsN, n + 1);
				}
			}
			g_clsKey[h] = vtable;
			g_clsVal[h] = (unsigned short)id;
			++g_clsKeys;
			return id;
		}
		h = (h + 1) & (CLASS_HASH - 1);
	}
	return 0;
}

// Mesh id of a Mesh*; 0 when unnamed or the table is full.
int MeshIdOf(const void* mesh)
{
	unsigned h = HashPtr(mesh, MESH_HASH - 1);
	for (int probe = 0; probe < MAX_PROBES_PER_LOOKUP; ++probe)
	{
		const void* k = g_meshKey[h];
		if (k == mesh)
			return g_meshVal[h];
		if (!k)
		{
			// Once the name table is full nothing new can be named: don't
			// cache further keys either.
			if (g_meshKeys >= MESH_HASH / 2 || g_meshN >= MAX_MESHES)
				return 0;
			int id = 0;
			LONG n = g_meshN;
			if (g_resGetName)
			{
				NameEntry& e = g_meshE[n];
				memset(&e, 0, sizeof(e));
				e.key = mesh;
				CopyOgreString(g_resGetName(mesh), e.name, NAME_LEN);
				if (e.name[0])
				{
					id = (int)n;
					_WriteBarrier();
					InterlockedExchange(&g_meshN, n + 1);
				}
			}
			g_meshKey[h] = mesh;
			g_meshVal[h] = (unsigned short)id;
			++g_meshKeys;
			return id;
		}
		h = (h + 1) & (MESH_HASH - 1);
	}
	return 0;
}

// Cameras and viewports are few and long-lived: a linear table.
int NamedId(NameEntry* table, volatile LONG* count, const void* key, bool camera)
{
	if (!key)
		return 0;
	LONG n = *count;
	for (LONG i = 1; i < n; ++i)
	{
		if (table[i].key == key)
			return (int)i;
	}
	if (n >= MAX_NAMED)
		return 0;
	NameEntry& e = table[n];
	memset(&e, 0, sizeof(e));
	e.key = key;
	if (camera)
	{
		if (g_moGetName)
			CopyOgreString(g_moGetName(key), e.name, NAME_LEN);
		if (!e.name[0])
			_snprintf_s(e.name, NAME_LEN, _TRUNCATE, "camera@%p", key);
	}
	else
	{
		char rt[40];
		rt[0] = 0;
		const void* target = g_vpTarget ? g_vpTarget(key) : NULL;
		if (target && g_rtGetName)
			CopyOgreString(g_rtGetName(target), rt, sizeof(rt));
		int w = g_vpWidth ? g_vpWidth(key) : 0;
		int hgt = g_vpHeight ? g_vpHeight(key) : 0;
		_snprintf_s(e.name, NAME_LEN, _TRUNCATE, "%s:%dx%d#%ld", rt[0] ? rt : "rt?", w, hgt, (long)n);
	}
	// These names go into key=value fields: no spaces.
	for (char* ch = e.name; *ch; ++ch)
	{
		if (*ch == ' ' || *ch == '\t')
			*ch = '_';
	}
	_WriteBarrier();
	InterlockedExchange(count, n + 1);
	return (int)n;
}

} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;

namespace kenshiframeaudit_detail {

void* MainSceneManager()
{
	uintptr_t holder = *(uintptr_t*)(KlibAddress(g_base, RVA_RENDERER));
	if (!PlausiblePtr(holder))
		return NULL;
	uintptr_t sm = *(uintptr_t*)(KLIB_MEMBER(5, holder, Renderer_scene, RENDERER_SCENEMGR));
	return PlausiblePtr(sm) ? (void*)sm : NULL;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
