#ifndef KEO_NM_BUSY_BRIDGE_POLICY_H
#define KEO_NM_BUSY_BRIDGE_POLICY_H

// The generator's busy bridge: a count of jobs a mod thread has claimed and
// not finished, and the generator byte the game reads as "generating" (+265).
// Each transition runs under the generator's queue lock (+152), the lock a
// claim holds while it unlinks its job, so at every release of that lock the
// byte reads 0 only while the count is 0. Nothing is taken under the queue
// lock here. Pure: the lock and the two words come in through BusyBridgeOps,
// so the host suite drives this code with its own.

struct BusyBridgeOps
{
	void* ctx;
	void (*lockQueue)(void* ctx);
	void (*unlockQueue)(void* ctx);
	long (*increment)(void* ctx);           // returns the new count
	long (*decrement)(void* ctx);           // returns the new count
	long (*readCount)(void* ctx);
	unsigned char (*readFlag)(void* ctx);
	void (*writeFlag)(void* ctx, unsigned char value);
};

enum BusyBridgeOp
{
	BUSY_BRIDGE_ENTER,          // a claim: count up, byte 1
	BUSY_BRIDGE_LEAVE,          // a finished claim: count down, byte 0 at zero
	BUSY_BRIDGE_CLEAR_IF_IDLE   // the backstop: byte 0 when nothing is claimed
};

// lockHeld: the caller already holds the queue lock; otherwise it is taken
// here (for BUSY_BRIDGE_CLEAR_IF_IDLE only when the byte reads 1, so an idle
// poll costs no lock). Returns false when the transition ends, still under
// the lock, with the byte 0 and the count not 0.
bool BusyBridge(const BusyBridgeOps& ops, BusyBridgeOp op, bool lockHeld);

// A finished claim, from a thread that holds no lock.
bool BusyBridgeLeave(const BusyBridgeOps& ops);

#endif
