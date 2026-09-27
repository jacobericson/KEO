#include "render/foliage_budget.h"

void FoliageBudgetReset(FoliageBudget* b)
{
	b->start = 0;
	b->index = 0;
	b->resumeAt = -1;
	b->budgeted = false;
	b->spentMs = 0.0;
	b->frameMaxMs = 0.0;
}

bool FoliageBudgetAdmit(FoliageBudget* b, bool active, double budgetMs)
{
	int k = b->index++;
	if (!active)
		return true;
	b->budgeted = true;
	if (k == 0 || k == b->start)
		return true;
	if (k < b->start || b->resumeAt >= 0)
		return false;
	if (b->spentMs >= budgetMs)
	{
		b->resumeAt = k;
		return false;
	}
	return true;
}

void FoliageBudgetSpend(FoliageBudget* b, double ms)
{
	b->spentMs += ms;
}

// A frame that ran out of budget resumes where it stopped; any other frame
// (unbudgeted, finished the list, or saw fewer calls than start because the
// list shrank) starts the next one from the top.
void FoliageBudgetEndFrame(FoliageBudget* b)
{
	b->start = (b->budgeted && b->resumeAt >= 0) ? b->resumeAt : 0;
	if (b->spentMs > b->frameMaxMs)
		b->frameMaxMs = b->spentMs;
	b->index = 0;
	b->resumeAt = -1;
	b->budgeted = false;
	b->spentMs = 0.0;
}
