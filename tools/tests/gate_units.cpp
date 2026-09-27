#include <cstdio>
#include "pathfind/gate_loop.h"

#include "check.h"

int main()
{
	Check(GateLoopFromReturnRva(0x2EF6EF) == GATE_LOOP_ENABLED,  "per-gate return");
	Check(GateLoopFromReturnRva(0x2EF88F) == GATE_LOOP_INTERIOR, "interior return");
	Check(GateLoopFromReturnRva(0x2EFB10) == GATE_LOOP_FIX,      "external-fix return");
	Check(GateLoopFromReturnRva(0x2EF6EA) == GATE_LOOP_OTHER,    "a call address is not a return address");
	Check(GateLoopFromReturnRva(0) == GATE_LOOP_OTHER,           "zero");
	return CheckExit("gate_units");
}
