#include "zone/handoff/zone_adoption_latency.h"

void ZoneAdoptionLatencyInit(ZoneAdoptionLatency* a)
{
	ZonePercentileReset(&a->readyToAdmitted);
	ZonePercentileReset(&a->admittedToActive);
	ZonePercentileReset(&a->readyToActive);
}

void ZoneAdoptionLatencyRecord(ZoneAdoptionLatency* a, double readyAt, double admittedAt, double activeAt)
{
	ZonePercentileAdd(&a->readyToAdmitted, admittedAt - readyAt);
	ZonePercentileAdd(&a->admittedToActive, activeAt - admittedAt);
	ZonePercentileAdd(&a->readyToActive, activeAt - readyAt);
}

double ZoneAdoptionLatencyP99ReadyToActive(const ZoneAdoptionLatency* a)
{
	return ZonePercentileValue(&a->readyToActive, 99);
}

double ZoneAdoptionLatencyP99ReadyToAdmitted(const ZoneAdoptionLatency* a)
{
	return ZonePercentileValue(&a->readyToAdmitted, 99);
}

double ZoneAdoptionLatencyP99AdmittedToActive(const ZoneAdoptionLatency* a)
{
	return ZonePercentileValue(&a->admittedToActive, 99);
}

int ZoneAdoptionLatencySampleCount(const ZoneAdoptionLatency* a)
{
	return a->readyToActive.n;
}
