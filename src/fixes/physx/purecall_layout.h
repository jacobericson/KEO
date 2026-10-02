#ifndef KEO_FIXES_PURECALL_LAYOUT_H
#define KEO_FIXES_PURECALL_LAYOUT_H

// Pure layout facts about PhysXCore64.dll's statically linked CRT and the
// address arithmetic the recorder needs. No Windows header, no I/O: kept
// host-testable under tools/tests/.

// Steam 1.0.65 PhysXCore64.dll RVAs, verified in IDA (PhysXCore64.dll.i64):
// _purecall itself, and the encoded handler-pointer global it
// reads (qword_18040AF80) before tail-jumping into abort().
extern const unsigned __int64 kPurecallRva;
extern const unsigned __int64 kPurecallHandlerPtrRva;

// The first 11 bytes at kPurecallRva: "sub rsp,28h" (4 bytes), then
// "mov rcx,[rip+disp32]" (7 bytes) whose displacement resolves to
// kPurecallHandlerPtrRva. One match proves both addresses together: if a
// PhysX build moves either, the displacement stops resolving to the other
// and the bytes differ.
extern const int kPurecallSignatureLen;
extern const unsigned char kPurecallSignature[11];

bool PurecallSignatureMatches(const unsigned char* bytes, int len);

// _purecall calls the decoded handler with a plain `call rax`, not a tail
// call (verified against the whole function body: no push, no other stack
// traffic between entry and that call). So the handler's own return-address
// slot holds an address inside _purecall; _purecall's own caller -- the real
// faulting call site -- sits one frame further up: 8 bytes for that call's
// own return address, plus the 0x28 _purecall subtracted from rsp at its own
// entry. Valid only while the signature above still matches this build.
extern const unsigned __int64 kCulpritSlotOffset;
unsigned __int64 CulpritSlotAddress(unsigned __int64 handlerReturnSlotAddr);

bool AddressInRange(unsigned __int64 addr, unsigned __int64 base, unsigned __int64 size);

enum PurecallModuleClass { PURECALL_MOD_UNKNOWN = 0, PURECALL_MOD_EXE, PURECALL_MOD_PHYSX };

// Classifies a culprit address against the two module ranges the recorder
// already knows (resolved once at install; see purecall_record.cpp).
PurecallModuleClass ClassifyPurecallCulprit(unsigned __int64 addr,
	unsigned __int64 gameBase, unsigned __int64 gameSize,
	unsigned __int64 physxBase, unsigned __int64 physxSize);

// Retry policy for the deferred install (purecall_record.cpp):
// PhysXCore64.dll loads after startPlugin returns in the normal case, not as
// an edge case, so a single failed GetModuleHandleA at plugin init must not
// mean "never armed for the session." Retries are throttled so a session
// where PhysX never loads (or loads a build the signature rejects) doesn't
// probe every frame, and bounded so it doesn't probe forever.
extern const double kPurecallRetryIntervalSec;
extern const double kPurecallRetryWindowSec;

// True once at least kPurecallRetryIntervalSec has passed since the last
// attempt (lastAttempt < 0 means "no attempt yet", always due).
bool PurecallRetryDue(double now, double lastAttempt);

// True once kPurecallRetryWindowSec has elapsed since the first attempt with
// no successful install -- the caller logs once and stops retrying.
bool PurecallRetryExpired(double now, double firstAttempt);

#endif // KEO_FIXES_PURECALL_LAYOUT_H
