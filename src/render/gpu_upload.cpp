#include "render/render_levers.h"
#include "render/render_config.h"
#include "render/module_hooks.h"
#include "render/upload_shadow.h"
#include "render/upload_plan.h"
#include "game/klib_member_contract.h"
#include "base/core.h"
#include <iomanip>
#include <sstream>

// D3D11HLSLProgram's constant-buffer upload (not exported), called once per
// shader stage from bindGpuProgramParameters, which binds the buffer it
// returns. It locks the program's first uniform buffer with DISCARD (a Map
// with WRITE_DISCARD), copies each shader variable's bytes from the
// parameters' float or int array straight into the mapped memory, unlocks it
// (the Unmap), drops the parameters reference it was passed by value and
// returns the ID3D11Buffer. The lock and unlock leave the buffer object's
// own state as they found it.
//
// Both keys keep, per buffer, a copy of the bytes the GPU buffer holds:
// - gpuUploadDiag replays the copy through the constant lookups and counts
//   the uploads that equal it. The original always runs.
// - gpuUploadSkip compares through a cached copy plan (upload_plan.h). When
//   nothing differs it does the rest of the function's work itself (drops
//   the reference, returns the buffer) and leaves the buffer unmapped, so it
//   keeps the bytes the upload would have written.
// The diagnostic wins when both are on.
//
// A copy is trusted only while every write to its buffer came through here
// on the main thread with a key on. A call on another thread or a call with
// both keys off sets s_invalidateCopies, and the next tracked call restarts
// every copy. A constant buffer destroyed sets s_invalidateAll, which also
// drops every plan: its address, and its program node's, can come back for
// a new, uninitialised buffer whose variables differ.

#ifdef KEO_DEBUG
static const bool DEV_BUILD = true;
#else
static const bool DEV_BUILD = false;
#endif

// The program and the structures the upload reads, at the offsets it reads them.
static const size_t PROG_BUFFER_HEAD  = 0x450;  // list head; *head is the first buffer's node
static const size_t PROG_BUFFER_COUNT = 0x458;
static const size_t NODE_UNIFORM      = 0x48;   // HardwareUniformBuffer*
static const size_t NODE_VARS_BEGIN   = 0x58;   // vector of 56-byte variable records
static const size_t NODE_VARS_END     = 0x60;
static const size_t VAR_STRIDE        = 56;     // name std::string at +0
static const size_t VAR_SIZE          = 0x28;
static const size_t VAR_OFFSET        = 0x30;
static const size_t HWBUF_SIZE        = 0x08;   // mSizeInBytes; the lock covers all of it
static const size_t UNIFORM_IMPL      = 0x70;   // D3D11HardwareBuffer*
static const size_t IMPL_D3D_BUFFER   = 0x40;   // ID3D11Buffer*
static const size_t IMPL_BUFFER_TYPE  = 0x5C;   // int; 2 = constant buffer
static const int    BUFFER_TYPE_CONSTANT = 2;
static const size_t PARAMS_FLOATS     = 0x58;   // float array data
static const size_t PARAMS_INTS       = 0x98;   // int array data
static const size_t SHAREDPTR_USES    = 0x20;   // 32-bit use count in the SharedPtr's info

static const size_t MAX_COMPARED_BYTES = 64 * 1024;
static const size_t MAX_PLAN_STEPS     = 4096;

typedef void* (*GetConstantBuffer_t)(void* program, void** paramsPtr);
typedef void* (*DestroyBuffer_t)(void* impl, int flags);
typedef bool  (*IsFloat_t)(int type);
typedef const void* (*GetConstDef_t)(const void* params, const void* name);

static const ModuleSite s_site =
{
	"D3D11HLSLProgram constant-buffer upload", "RenderSystem_Direct3D11_x64.dll", NULL,
	0x13EB0,
	{ 0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x83,0xEC,0x30 }
};

// D3D11HardwareBuffer's deleting destructor (this, flags): every one is
// destroyed through it, and it releases the ID3D11Buffer.
static const ModuleSite s_dtorSite =
{
	"D3D11HardwareBuffer deleting destructor", "RenderSystem_Direct3D11_x64.dll", NULL,
	0x20030,
	{ 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0xDA,0x48,0x8B,0xF9,0xE8 }
};

static const char* const IS_FLOAT_SYMBOL =
	"?isFloat@GpuConstantDefinition@Ogre@@SA_NW4GpuConstantType@2@@Z";
static const char* const GET_CONST_DEF_SYMBOL =
	"?getConstantDefinition@GpuProgramParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z";

static GetConstantBuffer_t s_orig      = NULL;
static DestroyBuffer_t     s_origDtor  = NULL;
static IsFloat_t           s_isFloat   = NULL;
// The OgreMain export itself, never the D3D11 import slot the constant cache
// patches: these lookups leave the cache and its counters alone.
static GetConstDef_t       s_getConstDef = NULL;
static int                 s_hookState = -1;    // the upload hook: -1 untried, 0 refused, 1 live
static int                 s_skipState = -1;    // the skip's other hooks, likewise
static volatile bool       s_skipReady = false;

// Main thread only. The slot array, every copy and every plan are allocated
// while a key is on and freed when both go off: at most 8,192 buffers and
// 64 MB of copies and plans (each copy at most MAX_COMPARED_BYTES); past
// either, a new buffer is counted as not compared.
static const int    SHADOW_SLOTS     = 8192;
static const size_t SHADOW_MAX_BYTES = 64 * 1024 * 1024;
static UploadShadowTable s_table;
static volatile LONG     s_invalidateCopies = 1;
static volatile LONG     s_invalidateAll = 0;

// Main thread only, apart from s_offThread.
static LONG          s_calls = 0, s_identical = 0;          // gpuUploadDiag
static LONGLONG      s_identicalBytes = 0;
static LONG          s_skipCalls = 0, s_skipped = 0;        // gpuUploadSkip
static LONG          s_planBuilds = 0;
static LONG          s_notCompared = 0, s_resets = 0;
static volatile LONG s_offThread = 0;

static bool DiagOn() { return DEV_BUILD && g_renderCfg.gpuUploadDiag; }
static bool SkipOn() { return s_skipReady && g_renderCfg.gpuUploadSkip; }

// Compares every variable's source bytes with the copy and stores them.
// Returns the bytes compared, or -1 when a variable lies outside the buffer
// or a lookup threw (the original then throws at the same variable, after its
// own Map). identical is false when any byte differed or the copy held no
// previous upload. Leaves the copy invalid: it is valid once the original
// has uploaded the bytes it now holds.
static LONGLONG CompareAndStore(UploadShadow* s, const char* vars, size_t count,
                                const char* params, bool* identical)
{
	bool differs = !s->valid;
	LONGLONG compared = 0;
	s->valid = false;
	try
	{
		for (size_t v = 0; v < count; ++v)
		{
			const char* var = vars + v * VAR_STRIDE;
			size_t bytes = *(const size_t*)(var + VAR_SIZE);
			size_t offset = *(const size_t*)(var + VAR_OFFSET);
			const char* def = (const char*)s_getConstDef(params, var);
			const char* array = *(const char* const*)(params +
				(s_isFloat(*(const int*)(def + UPLOAD_DEF_TYPE)) ? PARAMS_FLOATS : PARAMS_INTS));
			const char* src = array + 4 * *(const size_t*)(def + UPLOAD_DEF_PHYS_INDEX);
			if (!UploadShadowUpdate(s, offset, src, bytes, &differs))
				return -1;
			compared += (LONGLONG)bytes;
		}
	}
	catch (...)
	{
		return -1;
	}
	*identical = !differs;
	return compared;
}

// The program's first buffer node, or NULL when the original uploads nothing
// (no buffer); *uniform is then its uniform buffer.
static const char* FirstBufferNode(void* program, const char** uniform)
{
	const char* prog = (const char*)program;
	if (*(const size_t*)(prog + PROG_BUFFER_COUNT) == 0)
		return NULL;
	const char* node = **(const char* const* const*)(prog + PROG_BUFFER_HEAD);
	*uniform = *(const char* const*)(node + NODE_UNIFORM);
	return *uniform ? node : NULL;
}

// The copy for this upload's buffer, or NULL when it can't be compared.
static UploadShadow* AcquireCopy(const char* node, const char* uniform, const char* params,
                                 void** bufferOut)
{
	const char* impl = *(const char* const*)(uniform + UNIFORM_IMPL);
	size_t size = *(const size_t*)(uniform + HWBUF_SIZE);
	if (!impl || !params || size == 0 || size > MAX_COMPARED_BYTES)
		return NULL;
	void* buffer = *(void* const*)(impl + IMPL_D3D_BUFFER);
	*bufferOut = buffer;
	return s_table.Acquire(buffer, node, size);
}

// Main thread, inside the render pass: allocates a buffer's copy once, logs
// nothing. Returns the copy that will hold this upload's bytes once the
// original has run, or NULL.
static UploadShadow* CountUpload(void* program, void** paramsPtr)
{
	const char* uniform = NULL;
	const char* node = FirstBufferNode(program, &uniform);
	if (!node)
		return NULL;
	++s_calls;
	const char* params = (const char*)paramsPtr[0];
	void* buffer = NULL;
	UploadShadow* s = AcquireCopy(node, uniform, params, &buffer);
	if (!s)
	{
		++s_notCompared;
		return NULL;
	}
	const char* vars = *(const char* const*)(node + NODE_VARS_BEGIN);
	size_t count = (size_t)(*(const char* const*)(node + NODE_VARS_END) - vars) / VAR_STRIDE;
	bool identical = false;
	LONGLONG compared = CompareAndStore(s, vars, count, params, &identical);
	if (compared < 0)
	{
		++s_notCompared;
		return NULL;
	}
	if (identical)
	{
		++s_identical;
		s_identicalBytes += compared;
	}
	return s;
}

enum PlanBuild { PLAN_BUILT, PLAN_REFUSED, PLAN_THREW };

// Resolves every variable's definition once, through the OgreMain export.
// PLAN_REFUSED when the plan can never be made for this key (too many
// variables, the byte cap, a variable outside the buffer); PLAN_THREW when a
// lookup threw -- the original then throws at the same variable. Either way
// the plan and the copy are left invalid.
static PlanBuild BuildPlan(UploadShadow* s, const char* params, const char* vars, const char* varsEnd,
                           const void* map, long mapGen)
{
	++s_planBuilds;
	size_t count = (size_t)(varsEnd - vars) / VAR_STRIDE;
	if (count > MAX_PLAN_STEPS || !s_table.ReservePlan(s, (int)count))
		return PLAN_REFUSED;
	try
	{
		for (size_t v = 0; v < count; ++v)
		{
			const char* var = vars + v * VAR_STRIDE;
			const char* def = (const char*)s_getConstDef(params, var);
			int type = *(const int*)(def + UPLOAD_DEF_TYPE);
			if (!UploadPlanAdd(s, *(const size_t*)(var + VAR_OFFSET), *(const size_t*)(var + VAR_SIZE),
			                   def, type, s_isFloat(type)))
				return PLAN_REFUSED;
		}
	}
	catch (...)
	{
		return PLAN_THREW;
	}
	UploadPlanCommit(s, map, vars, varsEnd, mapGen);
	return PLAN_BUILT;
}

// The original's release of the parameters reference it was passed (the
// use count, then both words of the SharedPtr nulled), done only while
// another reference remains -- the caller holds one -- so this is never the
// last one and never has to destroy anything. False when it would be.
static bool DropParamsRef(void** paramsPtr)
{
	volatile LONG* uses = (volatile LONG*)((char*)paramsPtr[1] + SHAREDPTR_USES);
	for (;;)
	{
		LONG n = *uses;
		if (n <= 1)
			return false;
		if (InterlockedCompareExchange(uses, n - 1, n) == n)
			break;
	}
	paramsPtr[1] = NULL;
	paramsPtr[0] = NULL;
	return true;
}

// Main thread, inside the render pass: no logging; a copy or plan is
// allocated once per buffer. Returns the buffer when the upload is left
// out. Otherwise returns NULL, with *pending set to the copy that will hold
// this upload's bytes once the original has run (or NULL).
static void* TrySkip(void* program, void** paramsPtr, UploadShadow** pending)
{
	*pending = NULL;
	const char* uniform = NULL;
	const char* node = FirstBufferNode(program, &uniform);
	if (!node)
		return NULL;
	++s_skipCalls;
	const char* params = (const char*)paramsPtr[0];
	void* buffer = NULL;
	UploadShadow* s = params && paramsPtr[1] ? AcquireCopy(node, uniform, params, &buffer) : NULL;
	if (!s)
	{
		++s_notCompared;
		return NULL;
	}
	const char* vars = *(const char* const*)(node + NODE_VARS_BEGIN);
	const char* varsEnd = *(const char* const*)(node + NODE_VARS_END);
	// The map is read before the deletion count: a map freed and replaced at
	// the same address was counted before params could point at it.
	const void* map = *(const void* const volatile*)(params + KLIB_OFF_GpuProgramParameters_namedConstants);
	long mapGen = GpuNamedConstantsDeleted();
	if (!UploadPlanMatches(s, map, vars, varsEnd, mapGen))
	{
		// A new plan starts from an invalid copy (ReservePlan): its ranges
		// may cover bytes the copy never took.
		PlanBuild b = UploadPlanRefusedFor(s, map, vars, varsEnd, mapGen)
			? PLAN_REFUSED : BuildPlan(s, params, vars, varsEnd, map, mapGen);
		if (b != PLAN_BUILT)
		{
			if (b == PLAN_REFUSED)
				UploadPlanRefuse(s, map, vars, varsEnd, mapGen);
			s->valid = false;
			++s_notCompared;
			return NULL;
		}
	}
	UploadCompare c = UploadPlanCompare(s, *(const char* const*)(params + PARAMS_FLOATS),
	                                    *(const char* const*)(params + PARAMS_INTS));
	if (c == UC_STALE)
	{
		++s_notCompared;
		return NULL;
	}
	if (c == UC_SAME && DropParamsRef(paramsPtr))
	{
		++s_skipped;
		return buffer;
	}
	s->valid = false;
	*pending = s;
	return NULL;
}

static void* hook_GetConstantBuffer(void* program, void** paramsPtr)
{
	bool diag = DiagOn();
	bool skip = !diag && SkipOn();
	if (!diag && !skip)
	{
		if (!s_invalidateCopies)
			InterlockedExchange(&s_invalidateCopies, 1);
		return s_orig(program, paramsPtr);
	}
	if (!IsMainThread())
	{
		InterlockedIncrement(&s_offThread);
		InterlockedExchange(&s_invalidateCopies, 1);
		return s_orig(program, paramsPtr);
	}
	if (s_invalidateAll)
	{
		InterlockedExchange(&s_invalidateAll, 0);
		InterlockedExchange(&s_invalidateCopies, 0);
		s_table.InvalidateAll();
		++s_resets;
	}
	else if (s_invalidateCopies)
	{
		InterlockedExchange(&s_invalidateCopies, 0);
		s_table.InvalidateCopies();
		++s_resets;
	}
	UploadShadow* pending = NULL;
	if (diag)
	{
		pending = CountUpload(program, paramsPtr);
	}
	else
	{
		void* buffer = TrySkip(program, paramsPtr, &pending);
		if (buffer)
			return buffer;
	}
	void* result = s_orig(program, paramsPtr);
	if (pending)
		pending->valid = true;
	return result;
}

// Any thread: a constant buffer going away may hand its address to a new one.
static void* hook_DestroyBuffer(void* impl, int flags)
{
	if (impl && *(const int*)((const char*)impl + IMPL_BUFFER_TYPE) == BUFFER_TYPE_CONSTANT)
		InterlockedExchange(&s_invalidateAll, 1);
	return s_origDtor(impl, flags);
}

// Idempotent: the upload hook both keys share.
static bool InstallUploadHook()
{
	if (s_hookState >= 0)
		return s_hookState == 1;
	s_hookState = 0;
	HMODULE ogre = GetModuleHandleA("OgreMain_x64.dll");
	s_isFloat = ogre ? (IsFloat_t)GetProcAddress(ogre, IS_FLOAT_SYMBOL) : NULL;
	s_getConstDef = ogre ? (GetConstDef_t)GetProcAddress(ogre, GET_CONST_DEF_SYMBOL) : NULL;
	if (!s_isFloat || !s_getConstDef)
	{
		LogMsg("Render: gpuUpload: OgreMain export missing: isFloat or getConstantDefinition");
		return false;
	}
	s_table.Configure(SHADOW_SLOTS, SHADOW_MAX_BYTES);
	if (!InstallModuleHook(s_site, (void*)&hook_GetConstantBuffer, (void**)&s_orig, NULL))
		return false;
	s_hookState = 1;
	return true;
}

bool InstallGpuUploadDiag()
{
	return InstallUploadHook();
}

// The skip also has to see every constant buffer destroyed and every
// GpuNamedConstants deleted; it stays off unless all its hooks are live.
bool InstallGpuUploadSkip()
{
	if (s_skipState >= 0)
		return s_skipState == 1;
	s_skipState = 0;
	if (!InstallUploadHook() || !InstallGpuNamedConstantsWatch())
		return false;
	if (!InstallModuleHook(s_dtorSite, (void*)&hook_DestroyBuffer, (void**)&s_origDtor, NULL))
		return false;
	s_skipState = 1;
	// Copies and plans made before the destructor hook was live are not trusted.
	InterlockedExchange(&s_invalidateAll, 1);
	s_skipReady = true;
	return true;
}

static void ResetCounts()
{
	s_calls = s_identical = 0;
	s_identicalBytes = 0;
	s_skipCalls = s_skipped = s_planBuilds = 0;
	s_notCompared = s_resets = 0;
	InterlockedExchange(&s_offThread, 0);
}

// Main thread: both keys off frees every copy and plan, so switching either
// back on starts from first sight.
void GpuUpload_MainThreadTick()
{
	static bool wasOn = false;
	if (s_hookState != 1)
		return;
	bool on = DiagOn() || SkipOn();
	if (wasOn && !on)
	{
		s_table.Clear();
		ResetCounts();
	}
	wasOn = on;
}

std::string GpuUploadStatsToken(LONG loops)
{
	LONG offThread = InterlockedExchange(&s_offThread, 0);
	LONG calls = s_calls, identical = s_identical;
	LONGLONG bytes = s_identicalBytes;
	LONG skipCalls = s_skipCalls, skipped = s_skipped, planBuilds = s_planBuilds;
	LONG notCompared = s_notCompared, resets = s_resets;
	ResetCounts();
	bool diag = DiagOn();
	bool skipKey = SkipOn();
	if (s_hookState != 1 || (!diag && !skipKey))
		return std::string();
	double perLoop = loops > 0 ? 1.0 / (double)loops : 0.0;
	std::ostringstream ss;
	ss.setf(std::ios::fixed);
	ss << std::setprecision(1);
	if (diag)
		ss << " upload=" << (double)calls * perLoop << "/"
		   << (double)identical * perLoop << "/" << (double)bytes * perLoop << "/loop";
	if (skipKey)
		ss << " uploadSkip=" << (double)skipCalls * perLoop << "/" << (double)skipped * perLoop << "/loop"
		   << " uploadPlan=" << planBuilds;
	if (notCompared)
		ss << " uploadNotCompared=" << notCompared;
	if (offThread)
		ss << " uploadOffThread=" << offThread;
	if (resets)
		ss << " uploadReset=" << resets;
	return ss.str();
}
