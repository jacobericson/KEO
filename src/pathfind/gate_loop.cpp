#include "pathfind/gate_loop.h"

int GateLoopFromReturnRva(unsigned long long rva)
{
	switch (rva)
	{
	case 0x2EF6EF: return GATE_LOOP_ENABLED;
	case 0x2EF88F: return GATE_LOOP_INTERIOR;
	case 0x2EFB10: return GATE_LOOP_FIX;
	default:       return GATE_LOOP_OTHER;
	}
}
