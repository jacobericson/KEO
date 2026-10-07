// byte_check_policy.h - The install-time byte rows of the inventory lane, pure: a row's bytes
// against the bytes this build carries, the refusal text naming the bytes read, and one install's
// record of its rows. No Windows, KenshiLib or game header; any thread, no lock, no allocation.
#ifndef KEO_INVENTORY_BYTE_CHECK_POLICY_H
#define KEO_INVENTORY_BYTE_CHECK_POLICY_H

namespace keo_inventory {

// An exact row (a layout displacement, a call slot, a site the lane patches or retargets) must
// match every byte. A callee-head row (16 bytes; the lane only calls the function) passes as its
// original prologue, or as another plugin's E9 or FF 25 detour over a matching tail, under the
// build gate's shared-site rule; a callee row of any other length refuses.
enum ByteCheckKind    { BYTE_CHECK_EXACT, BYTE_CHECK_CALLEE_HEAD };
enum ByteCheckVerdict { BYTE_CHECK_REFUSED, BYTE_CHECK_ORIGINAL, BYTE_CHECK_SHARED };
ByteCheckVerdict ByteCheckJudge(ByteCheckKind kind, const unsigned char* actual,
                                const unsigned char* expect, int len);

// "<name> read=XX XX ..." over the first min(len, 16) bytes, NUL-terminated. Returns the
// characters written (no terminator counted), or 0 when n is too small for the whole text.
int  ByteReadFormat(const char* name, const unsigned char* bytes, int len, char* out, int n);

// One install's pass over its rows: the refusing row's text and the callee heads found behind
// another plugin's detour, comma-separated. Caller-owned; reset before the first row.
struct ByteRowLog
{
	char why[96];
	char shared[128];
	int  sharedLen;
};
void ByteRowLogReset(ByteRowLog* log);
// Judges one row. False on a refusal, with log->why holding "<name> read=..." (or the name alone,
// cut to fit, when the bytes do not fit) and log->shared emptied; a shared callee head is appended
// to log->shared (a name that does not fit is left out).
bool ByteRowCheck(ByteRowLog* log, const char* name, ByteCheckKind kind,
                  const unsigned char* actual, const unsigned char* expect, int len);
// log->shared, or "none" when no row was shared.
const char* ByteRowShared(const ByteRowLog* log);

} // namespace keo_inventory

#endif
