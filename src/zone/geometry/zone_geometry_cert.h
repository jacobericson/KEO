#ifndef KENSHI_ZONE_OPT_ZONE_GEOMETRY_CERT_H
#define KENSHI_ZONE_OPT_ZONE_GEOMETRY_CERT_H

// The geometry certificate: what a generation captured about the world's
// geometry before it read any of it, and the rule that decides whether the
// result it produced may still be published. Pure logic -- no game headers,
// no atomics, no clock -- so every rule is decided here and exercised from
// tools/tests/zone_geometry_cert_units.cpp. The atomic counter the snapshots
// come from, and the call sites that take them, live in
// zone_geometry_epoch.h.


// =========================================================================
// The mode, and why late adoption cannot be selected
// =========================================================================
//
// Adopting a cell against a mesh this mod generated means asserting its
// geometry inputs are still the ones the world has. The certificate below is
// that assertion's evidence, but evidence is only as good as its coverage:
// the epoch is bumped at the geometry boundaries the mod itself owns, and
// the engine's own building publication and removal paths are not among
// them. A certificate that is current therefore means "nothing the mod did
// moved geometry", which is weaker than "nothing moved geometry".
//
// So the mode is locked the way the adoption seam is locked -- structurally,
// not by a value that happens to be false. ZONE_GEOMETRY_ALLOW_LATE_ADOPT is
// the single named constant whose flip makes the production store point act
// on a certificate; defining it to anything else fails the build, and the
// INI parser refuses the mode by name as well, so no configuration can
// select it.
//
// Both arms of ZoneGeometryRejectsStore are live and host-tested. What the
// lock removes is the production site's ability to reach the second one.
#ifndef ZONE_GEOMETRY_ALLOW_LATE_ADOPT
#define ZONE_GEOMETRY_ALLOW_LATE_ADOPT 0
#endif

#if ZONE_GEOMETRY_ALLOW_LATE_ADOPT
#error "late adoption needs verified geometry producer coverage; extend the epoch's bump sites and remove this fence in the same change"
#endif

enum ZoneGeometryMode
{
	// Prepare content, assert nothing about any mesh. Every cell takes this
	// path today.
	ZONE_GEOMETRY_CONTENT_ONLY = 0,
	// Publish a generated mesh only against a certificate that is still
	// current. Selectable only by flipping the constant above.
	ZONE_GEOMETRY_LATE_ADOPT
};


// =========================================================================
// Snapshots and the capture fence
// =========================================================================
//
// A snapshot is one atomic read of the geometry counter: how many geometry
// mutations have completed (`epoch`), and how many are in flight right now
// (`mutationDepth`). A mutation raises the depth before it touches anything
// and, in the same atomic step, lowers the depth and raises the epoch when
// it is done -- so a completed mutation always moves the epoch, and a
// mutation that is still running always shows a depth.
//
// A generation captures a snapshot before it reads any geometry and the
// store point takes another after the result exists. The result may be
// published only when:
//
//   * the capture saw depth 0      -- no mutation was running when the read
//                                     began, so none can have been partly
//                                     visible to it;
//   * the epochs are equal         -- no mutation completed during the read;
//   * the store sees depth 0       -- no mutation is running now, so none
//                                     began during the read and is still
//                                     going.
//
// Those three cover every position a mutation can take relative to the
// window, whatever thread it runs on and however many generations are in
// flight: one wholly inside the window moves the epoch; one overlapping
// either end shows a depth at that end; one spanning the whole window shows
// a depth at both. None of the three needs the generation to hold a lock,
// which is what makes the fence correct against generations running with
// processJobCS released.

struct ZoneGeometrySnapshot
{
	unsigned epoch;
	unsigned mutationDepth;
};

// A certificate holds exactly the fields the check below reads. Payload
// elements the design also names -- per-source epochs, the support cohort,
// the interior/foliage inputs -- are absent on purpose: nothing produces
// them, and a field no predicate compares is a claim with no evidence
// behind it.
struct ZoneGeometryCertificate
{
	unsigned capturedEpoch;
	int      cellX;
	int      cellY;
	bool     valid;      // false when the capture itself raced a mutation
};

// Builds the certificate for a generation about to read geometry for this
// cell. A capture that lands while a mutation is in flight yields an invalid
// certificate and does not retry: the generation still runs, its result is
// still used for the job that asked for it, and only publication is refused.
ZoneGeometryCertificate ZoneGeometryCertificateCapture(const ZoneGeometrySnapshot& at,
                                                       int cellX, int cellY);

// A certificate that was never captured at all (a code path that reached the
// store without passing the capture site).
ZoneGeometryCertificate ZoneGeometryCertificateNone();

enum ZoneCertStaleness
{
	ZONE_CERT_CURRENT = 0,
	ZONE_CERT_NO_CAPTURE,     // no certificate was taken for this result
	ZONE_CERT_WRONG_CELL,     // the certificate belongs to another cell's job
	ZONE_CERT_CAPTURE_RACED,  // a mutation was in flight when the read began
	ZONE_CERT_EPOCH_MOVED,    // a mutation completed during the read
	ZONE_CERT_STORE_RACED     // a mutation is in flight now
};

ZoneCertStaleness ZoneGeometryCertificateCheck(const ZoneGeometryCertificate& cert,
                                               const ZoneGeometrySnapshot& atStore,
                                               int cellX, int cellY);

// Whether this verdict refuses publication. Under ZONE_GEOMETRY_CONTENT_ONLY
// nothing is refused -- the mod asserts nothing about the mesh, so a stale
// certificate describes a result the cache treats exactly as it always has,
// and the verdict is counted rather than acted on.
bool ZoneGeometryRejectsStore(ZoneGeometryMode mode, ZoneCertStaleness verdict);

const char* ZoneCertStalenessName(ZoneCertStaleness verdict);

// The parser's half of the lock: any spelling other than "contentOnly" is
// refused, and `lateAdopt` is refused by name rather than falling through to
// the default, so a configuration that asks for it is told so.
bool ZoneGeometryModeFromName(const char* name, ZoneGeometryMode* out);

#endif // KENSHI_ZONE_OPT_ZONE_GEOMETRY_CERT_H
