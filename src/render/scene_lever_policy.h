#ifndef KEO_RENDER_SCENE_LEVER_POLICY_H
#define KEO_RENDER_SCENE_LEVER_POLICY_H

// The scene switches' rules: which of the scene manager's worker forks would
// give every worker nothing to do, whether an instance batch's upload would be
// empty, the call-site stubs' bytes and the OgreMain build every offset here
// was read from. Pure: no game, Windows
// or Ogre header.

#include <stddef.h>
#include <stdint.h>

// OgreMain_x64.dll: its PE TimeDateStamp and SizeOfImage.
const unsigned long OGRE_SCENE_BUILD_STAMP = 0x5CA5F929UL;
const unsigned long OGRE_SCENE_BUILD_SIZE  = 0x9C9000UL;

// SceneManager: the per-thread visible lists ({ data, size, capacity } each)
// and their count; the skeleton-animation and instance managers ({ begin, end }
// of pointers); the threaded-instancing word.
const size_t SM_VISIBLE_LISTS       = 0x4B48;
const size_t SM_VISIBLE_LIST_COUNT  = 0x4B50;
const size_t VISIBLE_LIST_STRIDE    = 24;
const size_t VISIBLE_LIST_SIZE      = 8;
const size_t SM_SKEL_MGRS_BEGIN     = 0x3F8;
const size_t SM_SKEL_MGRS_END       = 0x400;
const size_t SM_INST_MGRS_BEGIN     = 0x578;
const size_t SM_INST_MGRS_END       = 0x580;
const size_t SM_THREADED_INSTANCING = 0x4AF0;
// InstanceManager: its dynamic batches and its dirty static batches.
const size_t IM_DYNAMIC_BEGIN = 72;
const size_t IM_DYNAMIC_END   = 80;
const size_t IM_DIRTY_BEGIN   = 104;
const size_t IM_DIRTY_END     = 112;
// MovableObject's vtable slot for the threaded instance-batch cull.
const size_t VT_BATCH_CULL = 0x48;
// InstanceBatchHW: its scene manager and its culled-instance count.
const size_t BATCH_SCENE_MANAGER = 0xE8;
const size_t BATCH_CULLED_COUNT  = 0x390;

// Walk bounds: anything longer reads as work, and the fork runs.
const size_t FORK_MAX_LISTS    = 256;
const size_t FORK_MAX_OBJECTS  = 1 << 20;
const size_t FORK_MAX_MANAGERS = 256;

// The three forks; a fork's fire site is pair * 2, its wait pair * 2 + 1.
enum ForkPair { FORK_R8, FORK_R1, FORK_R7, FORK_PAIRS };
const int FORK_SITES = FORK_PAIRS * 2;

// Reads behind the import-thunk test; each answers false when the address
// cannot be read.
typedef bool (*CullReadBytesFn)(uintptr_t addr, unsigned char* out, size_t len);
typedef bool (*CullReadPtrFn)(uintptr_t addr, uintptr_t* out);
struct CullReaders
{
	CullReadBytesFn bytes;
	CullReadPtrFn   ptr;
};
// Slots already confirmed as thunks to the base. Code and import tables do
// not change after load, so an entry is never dropped; a full cache still
// answers by reading.
const size_t CULL_THUNK_CACHE = 8;
struct CullThunkCache
{
	uintptr_t slot[CULL_THUNK_CACHE];
	size_t    count;
};
// A cull slot is the base when it is baseCull itself, or a jmp [rip+disp32]
// (FF 25, or 48 FF 25) whose import entry holds baseCull: a module that
// inherits the base reaches it through its own import thunk. NULL, any other
// address and any read that fails answer false. With no readers only the
// equality is tested; with no cache nothing is remembered.
bool CullSlotIsBase(uintptr_t slot, uintptr_t baseCull, CullThunkCache* cache, const CullReaders* rd);
// Instance-batch cull: every visible object's cull slot is the base
// (CullSlotIsBase), MovableObject's empty body. Any override, from any
// module, is work the workers would run, and the fork runs.
bool ForkR8Idle(const unsigned char* sm, uintptr_t baseCull, CullThunkCache* cache, const CullReaders* rd);
// Skeleton animation: every manager's list is empty.
bool ForkR1Idle(const unsigned char* sm);
// Instance managers: none has a dynamic or a dirty static batch.
bool ForkR7Idle(const unsigned char* sm);

// One fork's two sites: a skipped fire leaves pending for its wait.
struct ForkPairState
{
	bool live;      // both sites point at their stubs
	bool pending;   // the fire skipped and its wait has not run
};
enum ForkAction { FORK_CALL, FORK_SKIP, FORK_CALL_ODD };
// FORK_CALL_ODD: a fire found pending still set; it is cleared and the fire calls.
ForkAction ForkFire(ForkPairState* s, bool on, bool mainThread, bool idle);
ForkAction ForkWait(ForkPairState* s, bool mainThread);

// The stub a site's rel32 points at: the scene manager's register into rdx,
// the site number into r8d, then a jump to entry.
enum ForkReg { FORK_REG_RSI, FORK_REG_R12, FORK_REG_RDI };
const size_t FORK_STUB_BYTES = 21;
// The bytes written, 0 when cap is too small or the register is unknown.
size_t BuildForkStub(unsigned char* out, size_t cap, ForkReg reg, int site, unsigned long long entry);
// The rel32 from the instruction ending at next to target; false out of reach.
bool Rel32To(unsigned long long next, unsigned long long target, int32_t* rel);
// window: the len bytes ending with the site's E8 rel32. Equal to expect, and
// the call at site lands on target.
bool ForkSiteMatches(const unsigned char* window, const unsigned char* expect, size_t len,
                     unsigned long long site, unsigned long long target);

// updateVertexBuffer's early answer for one call.
enum InstUpload { INST_RUN, INST_EMPTY, INST_UNTHREADED, INST_OFF_MAIN };
InstUpload InstUploadDecide(bool mainThread, const unsigned char* batch);

#endif // KEO_RENDER_SCENE_LEVER_POLICY_H
