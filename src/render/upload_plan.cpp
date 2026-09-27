#include "render/upload_plan.h"
#include <cstring>

static bool SameKey(const UploadShadow* s, const void* map, const void* vars,
                    const void* varsEnd, long mapGen)
{
	return s->planMap == map && s->planVars == vars && s->planVarsEnd == varsEnd && s->planMapGen == mapGen;
}

static void SetKey(UploadShadow* s, const void* map, const void* vars, const void* varsEnd, long mapGen)
{
	s->planMap = map;
	s->planVars = vars;
	s->planVarsEnd = varsEnd;
	s->planMapGen = mapGen;
}

bool UploadPlanMatches(const UploadShadow* s, const void* map, const void* vars,
                       const void* varsEnd, long mapGen)
{
	return s->planValid && SameKey(s, map, vars, varsEnd, mapGen);
}

bool UploadPlanRefusedFor(const UploadShadow* s, const void* map, const void* vars,
                          const void* varsEnd, long mapGen)
{
	return s->planRefused && SameKey(s, map, vars, varsEnd, mapGen);
}

void UploadPlanRefuse(UploadShadow* s, const void* map, const void* vars, const void* varsEnd, long mapGen)
{
	SetKey(s, map, vars, varsEnd, mapGen);
	s->planValid = false;
	s->planRefused = true;
	s->planCount = 0;
}

bool UploadPlanAdd(UploadShadow* s, size_t dst, size_t size, const void* def, int type, bool isFloat)
{
	if (s->planCount >= s->planCap || dst > s->size || size > s->size - dst)
		return false;
	UploadPlanStep& step = s->plan[s->planCount++];
	step.dst = dst;
	step.size = size;
	step.def = def;
	step.type = type;
	step.isFloat = isFloat;
	return true;
}

void UploadPlanCommit(UploadShadow* s, const void* map, const void* vars, const void* varsEnd, long mapGen)
{
	SetKey(s, map, vars, varsEnd, mapGen);
	s->planValid = true;
	s->planRefused = false;
}

UploadCompare UploadPlanCompare(UploadShadow* s, const char* floats, const char* ints)
{
	bool differs = !s->valid;
	const UploadPlanStep* step = s->plan;
	const UploadPlanStep* end = step + s->planCount;
	for (; step != end; ++step)
	{
		const char* def = (const char*)step->def;
		if (*(const int*)(def + UPLOAD_DEF_TYPE) != step->type)
		{
			s->planValid = false;
			s->valid = false;
			return UC_STALE;
		}
		const char* src = (step->isFloat ? floats : ints) + 4 * *(const size_t*)(def + UPLOAD_DEF_PHYS_INDEX);
		unsigned char* dst = s->bytes + step->dst;
		if (memcmp(dst, src, step->size) != 0)
		{
			differs = true;
			memcpy(dst, src, step->size);
		}
	}
	return differs ? UC_DIFFERS : UC_SAME;
}
