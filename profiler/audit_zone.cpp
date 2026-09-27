// audit_zone.cpp - Zone lifecycle timing.
// Main thread records the lifecycle call; no probe takes a lock, allocates, or logs.

#include "audit_detail.h"

namespace kenshiframeaudit_detail {
char hk_ZoneLifecycle(void* zone)
{
	if (!IsMain())
		return oZoneLifecycle(zone);
	bool timed = g_cur.open;
	LONGLONG t0 = timed ? Now() : 0;
	char r = oZoneLifecycle(zone);
	if ((unsigned char)r == 0)
	{
		Hulls_NoteZoneUnload(*(const int*)(KLIB_MEMBER(5, zone, ZoneMap_coordinates_x, ZONE_COORD_X)),
		                     *(const int*)(KLIB_MEMBER(5, zone, ZoneMap_coordinates_y, ZONE_COORD_Y)));
		if (timed)
		{
			++g_cur.unloads;
			g_cur.unloadTicks += Now() - t0;
		}
	}
	return r;
}
} // kenshiframeaudit_detail
using namespace kenshiframeaudit_detail;
