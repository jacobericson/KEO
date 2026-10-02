#ifndef KEO_ZONE_ADOPTION_SEAM_H
#define KEO_ZONE_ADOPTION_SEAM_H

// The decision rules behind zone adoption: the certificate seam, the town
// guard predicate, and the two admission predicates (proactive cohort
// admission and real-demand takeover). Pure logic — no game or KenshiLib
// headers, no allocation, no clock of its own — so every rule is decided
// once here and exercised from the host tests in
// tools/tests/zone_adoption_units.cpp.


// =========================================================================
// The certificate seam
// =========================================================================
//
// Adopting a cell *against a navmesh this mod prepared* means asserting that
// the mesh's geometry inputs are still the ones the world has. That assertion
// is a certificate, and a certificate is only worth anything with the claim,
// invalidation and acknowledged-removal protocol behind it. None of that
// protocol exists yet, so this build cannot make the assertion at all.
//
// The seam is closed structurally, not by a value that happens to be false:
//
//   * ZoneCertificateVerdict has exactly one enumerator. No expression
//     anywhere in the source can name a "proved" verdict, so no function can
//     return one and no branch can test for one.
//   * ZoneAdmissionVerdict likewise has no "adopt against our mesh" answer,
//     so a caller cannot act on a certificate even if one existed.
//   * ZONE_ADOPT_LATE_PATH is 0 and the fence below fails the build if it is
//     ever defined to anything else. Turning late adoption on is therefore a
//     deliberate edit to this file, which is where the protocol's
//     preconditions are written down, rather than a define on a command line.
//
// Cells are adopted regardless — content-only, which asserts nothing about
// any mesh: the cohort admits a cell only once the engine's own per-zone
// readiness already reports its mesh in, and the game's phase 4 then decides
// publication with its own poll.
#ifndef ZONE_ADOPT_LATE_PATH
#define ZONE_ADOPT_LATE_PATH 0
#endif

#if ZONE_ADOPT_LATE_PATH
#error "late adoption needs the navmesh claim/certificate protocol; implement it and remove this fence in the same change"
#endif

enum ZoneCertificateVerdict
{
	ZONE_CERT_UNPROVED = 0
};

// Always ZONE_CERT_UNPROVED. It takes no arguments because no input could
// make the answer anything else while the enum has one value.
ZoneCertificateVerdict ZoneAdoptionCertificate();


// =========================================================================
// Activation types and the town guard
// =========================================================================

// ZoneActivationType, as the engine indexes activatedCountdown[].
const int ZONE_ACTIVATION_CAMERA = 0;
const int ZONE_ACTIVATION_PLAYER = 1;
const int ZONE_ACTIVATION_TOWN   = 2;
const int ZONE_ACTIVATION_COUNT  = 3;

// A town refreshing its coverage passes the largest camera/player countdown
// over the cells it covers, and a nonpositive value means no cell in that
// coverage carries a lease at all. The engine substitutes a fixed default for
// it, which renews the whole coverage from nothing and can keep a town
// resident forever. Refuse exactly that refresh; every positive one, and
// every other activation type, is left alone.
//
// Written so a NaN timer refuses rather than passes.
bool ZoneTownGuardRefuses(int activationType, float deactivationTimer);


// =========================================================================
// Content finalization
// =========================================================================
//
// The engine's own rule, in one place because its polarity is the opposite
// of what the byte's name suggests: `ZoneMapContent::update` finalizes a
// cell's content only while its activation flag is SET, and clearing that
// flag is the first thing the finalize does. So a set flag means "the
// finalize still has to run", and a clear flag means "it already has" —
// never "ready for it".
//
// The mod's own load sets the flag (it goes through the same activate the
// engine uses), so a cell it has just taken always needs the call.
bool ZoneContentNeedsFinalize(bool terrainLoaded, bool activationFlagSet);

// The finalize has already run for this cell: the flag is clear.
bool ZoneContentIsFinalized(bool activationFlagSet);


// =========================================================================
// Proactive cohort admission
// =========================================================================

struct ZoneAdmissionInputs
{
	bool preparedAndReady;    // the ledger holds this cell's current incarnation at ReadyForAdoption
	bool contentInitialized;  // the content object is finalized, not a bare shell
	bool meshReportedIn;      // the engine's own unmodified per-zone readiness answer for this cell
	bool flagsPrivate;        // the cell reads +176 = 1, +177 = 0 right now
	bool reAdmissionAllowed;  // long enough since this cell's last admission
};

enum ZoneAdmissionVerdict
{
	ZONE_ADMIT_NO = 0,
	ZONE_ADMIT_CONTENT_ONLY   // hand the cell over; assert nothing about its mesh
};

// `meshReportedIn` is a requirement, not a tuning choice. Native phase 4
// publishes accessibility for every member of the active set at once, and
// accessibility decides character height as well as activation, so a cell
// admitted while its mesh is out can be published against no mesh and drop
// whoever stands on an elevated floor. Admitting only cells the engine
// already calls ready keeps that impossible whatever the readiness detour
// answers.
ZoneAdmissionVerdict ZoneAdmissionDecide(const ZoneAdmissionInputs& in);

// The per-frame half of admission: cell-independent conditions that must all
// hold before any cohort is inserted.
struct ZoneCohortWindow
{
	bool mainThread;          // tracking sets are mutated from the main thread only
	bool loaderIdle;          // loadingPhase == 0: no native phase is walking the set
	bool transitionQuiet;     // no transition bracket open and none waiting to close
	bool worldQuiet;          // no save load in progress and justLoadedAGame clear
	bool centralZoneValid;
	bool physicsQueuesClear;  // the four main-side counts are 0
};

bool ZoneCohortWindowOpen(const ZoneCohortWindow& w);

// A cell may not be re-admitted immediately after the engine has let it go:
// preparing, admitting, expiring and preparing again would spend a loading
// cycle per loop, and every cycle freezes the active set's countdowns for
// several frames.
bool ZoneReAdmissionAllowed(double now, double lastAdmittedAt, double minIntervalSec);


// =========================================================================
// Real-demand takeover
// =========================================================================

struct ZoneTakeoverInputs
{
	bool mainThread;
	bool modHoldsPrivately;   // the ledger holds this cell's current incarnation at a private stage
	bool flagsPrivate;        // the cell reads +176 = 1, +177 = 0, so the engine's own activate refused it
	bool loaderWalkingSetA;   // ZoneManager::loadingPhase is 2 or 3
};

enum ZoneTakeoverVerdict
{
	ZONE_TAKEOVER_NO = 0,
	ZONE_TAKEOVER_ADOPT,      // insert into the pending set and report the activation as succeeded
	ZONE_TAKEOVER_DEFER       // the loader is walking the set: record the request, rewind nothing
};

ZoneTakeoverVerdict ZoneTakeoverDecide(const ZoneTakeoverInputs& in);

#endif // KEO_ZONE_ADOPTION_SEAM_H
