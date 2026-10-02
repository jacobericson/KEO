#ifndef KEO_GATE_LOOP_H
#define KEO_GATE_LOOP_H

// The three loops of the gate-code pass, told apart by where Gates__findPath
// returns to inside Gates__updateCodes.
enum { GATE_LOOP_ENABLED = 0, GATE_LOOP_INTERIOR = 1, GATE_LOOP_FIX = 2, GATE_LOOP_OTHER = 3 };
int GateLoopFromReturnRva(unsigned long long rva);

#endif
