#include "fixes/world/nest_validation_policy.h"

bool NestValidationCellFromRecord(unsigned int rawX, unsigned int rawY, int* outCellIndex)
{
	if (rawX > 63 || rawY > 63)
		return false;
	*outCellIndex = (int)rawY + ((int)rawX << 6);
	return true;
}

NestValidationDecision NestValidationDecide(bool meshReady, bool wasPreviouslySkipped)
{
	NestValidationDecision d;
	if (!meshReady)
	{
		d.action = NESTVAL_SKIP;
		d.countSkip = !wasPreviouslySkipped;  // once per streak, not once per call
		d.countRevalidated = false;
	}
	else
	{
		d.action = NESTVAL_PROCEED;
		d.countSkip = false;
		d.countRevalidated = wasPreviouslySkipped;
	}
	return d;
}
