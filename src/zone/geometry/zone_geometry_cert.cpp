#include "zone/geometry/zone_geometry_cert.h"

#include <string.h>

ZoneGeometryCertificate ZoneGeometryCertificateCapture(const ZoneGeometrySnapshot& at,
                                                       int cellX, int cellY)
{
	ZoneGeometryCertificate cert;
	cert.capturedEpoch = at.epoch;
	cert.cellX         = cellX;
	cert.cellY         = cellY;
	cert.valid         = (at.mutationDepth == 0);
	return cert;
}

ZoneGeometryCertificate ZoneGeometryCertificateNone()
{
	ZoneGeometryCertificate cert;
	cert.capturedEpoch = 0;
	cert.cellX         = -1;
	cert.cellY         = -1;
	cert.valid         = false;
	return cert;
}

ZoneCertStaleness ZoneGeometryCertificateCheck(const ZoneGeometryCertificate& cert,
                                               const ZoneGeometrySnapshot& atStore,
                                               int cellX, int cellY)
{
	// Ordered so the answer names the earliest thing that went wrong: a
	// certificate from another cell says nothing about this one whatever the
	// epochs read, and a capture that raced is not evidence even if the
	// epoch has since settled back to it.
	// Grid coordinates are never negative, so the "none" certificate is
	// distinguishable from every captured one.
	if (cert.cellX < 0 || cert.cellY < 0)
		return ZONE_CERT_NO_CAPTURE;
	if (cert.cellX != cellX || cert.cellY != cellY)
		return ZONE_CERT_WRONG_CELL;
	if (!cert.valid)
		return ZONE_CERT_CAPTURE_RACED;
	if (cert.capturedEpoch != atStore.epoch)
		return ZONE_CERT_EPOCH_MOVED;
	if (atStore.mutationDepth != 0)
		return ZONE_CERT_STORE_RACED;
	return ZONE_CERT_CURRENT;
}

bool ZoneGeometryRejectsStore(ZoneGeometryMode mode, ZoneCertStaleness verdict)
{
	if (mode == ZONE_GEOMETRY_CONTENT_ONLY)
		return false;
	return verdict != ZONE_CERT_CURRENT;
}

const char* ZoneCertStalenessName(ZoneCertStaleness verdict)
{
	switch (verdict)
	{
	case ZONE_CERT_CURRENT:        return "current";
	case ZONE_CERT_NO_CAPTURE:     return "noCapture";
	case ZONE_CERT_WRONG_CELL:     return "wrongCell";
	case ZONE_CERT_CAPTURE_RACED:  return "captureRaced";
	case ZONE_CERT_EPOCH_MOVED:    return "epochMoved";
	case ZONE_CERT_STORE_RACED:    return "storeRaced";
	}
	return "?";
}

bool ZoneGeometryModeFromName(const char* name, ZoneGeometryMode* out)
{
	if (!name || !out)
		return false;
	if (strcmp(name, "contentOnly") == 0)
	{
		*out = ZONE_GEOMETRY_CONTENT_ONLY;
		return true;
	}
	// lateAdopt is refused here as well as fenced at compile time, so a
	// configuration that asks for it gets an answer instead of silence.
	return false;
}
