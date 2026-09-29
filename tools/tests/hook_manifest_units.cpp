// The hook manifest's rows per variant, and the want predicate over them.
// The unit runs as four suite rows: a DEV and a PROD build at the session
// ZONEHAND_STEP, plus a DEV build at each of the two earlier steps.
// Each expands the row list twice, with and without ZONEOPT_DEBUG defined,
// and checks both variants' row, counted, wanted, startup and module counts
// at its own ZONEHAND_STEP. Each also compares the want inputs against its
// own build's compiled config defaults. Stringising leaves the RVA constants
// unexpanded, so no game header is needed.

#include <stdio.h>
#include <string.h>
#include "plugin/hook_manifest_policy.h"
#include "base/config_values.h"

#include "check.h"

#ifdef ZONEOPT_DEBUG
#define HOOK_MANIFEST_UNITS_DEV_BUILD 1
#else
#define HOOK_MANIFEST_UNITS_DEV_BUILD 0
#endif

struct TestRow
{
	const char*   id;
	const char*   name;
	const char*   rva;
	HookRowKind   kind;
	HookInstaller installer;
	HookWant      want;
	unsigned      caps;
	unsigned char bytes[16];
};

#define HOOK_ROW(id, name, rva, kind, installer, want, caps, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15) \
	{ #id, name, #rva, kind, installer, want, (unsigned)(caps), { b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15 } },

#if !HOOK_MANIFEST_UNITS_DEV_BUILD
#define ZONEOPT_DEBUG 1
#endif

static const TestRow kDevRows[] =
{
#include "plugin/hook_manifest_rows.inc"
};

#undef ZONEOPT_DEBUG

static const TestRow kProdRows[] =
{
#include "plugin/hook_manifest_rows.inc"
};

#undef HOOK_ROW

#if HOOK_MANIFEST_UNITS_DEV_BUILD
#define ZONEOPT_DEBUG 1
#endif

static const int kDevCount  = (int)(sizeof(kDevRows) / sizeof(kDevRows[0]));
static const int kProdCount = (int)(sizeof(kProdRows) / sizeof(kProdRows[0]));

static char s_msg[512];

static const char* Msg1(const char* fmt, const char* a)
{
	_snprintf_s(s_msg, sizeof(s_msg), _TRUNCATE, fmt, a);
	return s_msg;
}

static const char* Msg3(const char* fmt, const char* a, const char* b, const char* c)
{
	_snprintf_s(s_msg, sizeof(s_msg), _TRUNCATE, fmt, a, b, c);
	return s_msg;
}

static bool HasName(const TestRow* rows, int n, const char* name)
{
	for (int i = 0; i < n; ++i)
		if (strcmp(rows[i].name, name) == 0)
			return true;
	return false;
}

// The 19 inputs, each with its field and the config global it is read from; a
// NULL global is a key a PROD build does not carry, or the int key
// CheckInputMapping flips on its own.
struct InputField
{
	const char*            name;
	bool HookWantInputs::* field;
	bool*                  global;
};

static const InputField kFields[] =
{
	{ "destroyListDiag",     &HookWantInputs::destroyListDiag,     &fixes::g_fixesCfg.destroyListDiagEnabled },
	{ "destroyListDefer",    &HookWantInputs::destroyListDefer,    &fixes::g_fixesCfg.destroyListDeferEnabled },
	{ "escapePauseGuard",    &HookWantInputs::escapePauseGuard,    &zone::g_zoneCfg.escapePauseGuardEnabled },
	{ "corpsePin",           &HookWantInputs::corpsePin,           &fixes::g_fixesCfg.corpsePinEnabled },
	{ "nestValidationGuard", &HookWantInputs::nestValidationGuard, &fixes::g_fixesCfg.nestValidationGuardEnabled },
	{ "unstitchGuard",       &HookWantInputs::unstitchGuard,       &fixes::g_fixesCfg.unstitchGuardEnabled },
	{ "graphVisitorGuard",   &HookWantInputs::graphVisitorGuard,   &fixes::g_fixesCfg.graphVisitorGuardEnabled },
	{ "graphExpandGuard",    &HookWantInputs::graphExpandGuard,    &fixes::g_fixesCfg.graphExpandGuardEnabled },
	{ "meshFaceGuard",       &HookWantInputs::meshFaceGuard,       &fixes::g_fixesCfg.meshFaceGuardEnabled },
	{ "navMeshLife",         &HookWantInputs::navMeshLife,         &fixes::g_fixesCfg.navMeshLifeEnabled },
#ifdef ZONEOPT_DEBUG
	{ "unstitchProbe",       &HookWantInputs::unstitchProbe,       &fixes::g_fixesCfg.unstitchProbeEnabled },
	{ "sectionKeyProbe",     &HookWantInputs::sectionKeyProbe,     &fixes::g_fixesCfg.sectionKeyProbeEnabled },
#else
	{ "unstitchProbe",       &HookWantInputs::unstitchProbe,       NULL },
	{ "sectionKeyProbe",     &HookWantInputs::sectionKeyProbe,     NULL },
#endif
	{ "movementAware",       &HookWantInputs::movementAware,       &zone::g_zoneCfg.movementAwareEnabled },
	{ "caching",             &HookWantInputs::caching,             &navmesh::g_navmeshCfg.cachingEnabled },
	{ "pathfindDiag",        &HookWantInputs::pathfindDiag,        &pathfind::g_pathfindCfg.pathfindDiagEnabled },
	{ "pathExtractGuard",    &HookWantInputs::pathExtractGuard,    &fixes::g_fixesCfg.pathExtractGuardEnabled },
	{ "sectionStamp",        &HookWantInputs::sectionStamp,        &fixes::g_fixesCfg.sectionStampEnabled },
	{ "gatePassDiag",        &HookWantInputs::gatePassDiag,        &pathfind::g_pathfindCfg.gatePassDiagEnabled },
	{ "graphHeuristicGuard", &HookWantInputs::graphHeuristicGuard, NULL },
};
static const int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

static int FieldIndex(const char* name)
{
	for (int i = 0; i < kFieldCount; ++i)
		if (strcmp(kFields[i].name, name) == 0)
			return i;
	return -1;
}

// The DEV defaults: every key true but unstitchProbe and graphHeuristicGuard.
static HookWantInputs DevDefaults()
{
	HookWantInputs in;
	for (int i = 0; i < kFieldCount; ++i)
		in.*kFields[i].field = true;
	in.unstitchProbe = false;
	in.graphHeuristicGuard = false;
	return in;
}

// The PROD defaults: destroyListDiag, gatePassDiag and sectionKeyProbe off as well.
static HookWantInputs ProdDefaults()
{
	HookWantInputs in = DevDefaults();
	in.destroyListDiag  = false;
	in.gatePassDiag     = false;
	in.sectionKeyProbe  = false;
	return in;
}

static bool IsUncountedInstaller(HookInstaller installer)
{
	return installer == HOOK_BY_LAZY || installer == HOOK_BY_RENDER || installer == HOOK_BY_GUI;
}

static void CheckVariant(const TestRow* rows, int n, const char* variant, int expectRows,
                         int expectCounted, int expectWanted, int expectStartup, int expectModule,
                         const HookWantInputs& in)
{
	char what[64];

	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s rows", variant);
	Check(n == expectRows, what);

	int counted = 0, wanted = 0, startup = 0, module = 0;
	for (int i = 0; i < n; ++i)
	{
		if (rows[i].want != HOOK_WANT_UNCOUNTED)
			++counted;
		if (HookWantEval(rows[i].want, in))
			++wanted;
		if (rows[i].installer == HOOK_BY_STARTUP)
			++startup;
		if (rows[i].installer == HOOK_BY_MODULE)
			++module;
	}
	printf("hook_manifest_units: ZONEHAND_STEP=%d %s: %d rows, %d counted, %d wanted, %d startup, %d module\n",
	       ZONEHAND_STEP, variant, n, counted, wanted, startup, module);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s counted", variant);
	Check(counted == expectCounted, what);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s wanted", variant);
	Check(wanted == expectWanted, what);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s startup rows", variant);
	Check(startup == expectStartup, what);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s module rows", variant);
	Check(module == expectModule, what);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s counted rows are the startup and module rows", variant);
	Check(startup + module == counted, what);

	for (int i = 0; i < n; ++i)
	{
		const TestRow& r = rows[i];
		Check(r.kind == HOOK_FATAL || r.kind == HOOK_DIAGNOSTIC, Msg1("row %s: kind", r.name));

		bool anyByte = false;
		for (int b = 0; b < 16; ++b)
			if (r.bytes[b] != 0)
				anyByte = true;
		Check(anyByte, Msg1("row %s: no prologue bytes", r.name));

		bool idOk = strncmp(r.rva, "RVA_", 4) == 0 && strncmp(r.id, "HOOK_", 5) == 0
		         && strcmp(r.id + 5, r.rva + 4) == 0;
		Check(idOk, Msg3("row %s: id %s does not match %s", r.name, r.id, r.rva));

		Check(IsUncountedInstaller(r.installer) == (r.want == HOOK_WANT_UNCOUNTED),
		      Msg1("row %s: counted by its installer", r.name));

		for (int j = 0; j < i; ++j)
		{
			if (strcmp(rows[j].name, r.name) == 0)
				Check(false, Msg1("duplicate name %s", r.name));
			if (strcmp(rows[j].id, r.id) == 0)
				Check(false, Msg1("duplicate id %s", r.id));
			if (strcmp(rows[j].rva, r.rva) == 0)
				Check(false, Msg1("duplicate rva %s", r.rva));
		}
	}

	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s first row", variant);
	Check(n > 0 && strcmp(rows[0].name, "showLoadingMessage") == 0, what);
	_snprintf_s(what, sizeof(what), _TRUNCATE, "%s last row", variant);
	Check(n > 0 && strcmp(rows[n - 1].name, "OptionsWindow::saveOptions") == 0, what);
}

// Exactly the four DEV-only rows separate the variants: a define that leaked
// from the first expansion into the second would make the counts agree.
static void CheckDevMinusProd()
{
	static const char* const kDevOnly[] =
		{ "deleteInstance", "clearanceResetKeys", "sectionCutLookup", "loadPhysXResource" };

	bool ok = kDevCount - kProdCount == 4;
	int extra = 0;
	for (int i = 0; i < kDevCount; ++i)
		if (!HasName(kProdRows, kProdCount, kDevRows[i].name))
			++extra;
	ok = ok && extra == 4;
	for (int k = 0; k < 4; ++k)
		ok = ok && HasName(kDevRows, kDevCount, kDevOnly[k])
		        && !HasName(kProdRows, kProdCount, kDevOnly[k]);
	for (int i = 0; i < kProdCount; ++i)
		ok = ok && HasName(kDevRows, kDevCount, kProdRows[i].name);
	Check(ok, "dev minus prod");
}

// Flip one field, or a pair, away from the DEV defaults: exactly the named rows
// change their want.
struct Flip
{
	const char* label;
	const char* fields[2];
	const char* rows[8];
};

static const Flip kFlips[] =
{
	{ "movementAware", { "movementAware" }, { "addOrderSelected" } },
	{ "destroyListDiag", { "destroyListDiag" }, { NULL } },
	{ "destroyListDefer", { "destroyListDefer" }, { NULL } },
	{ "destroyListDiag and destroyListDefer", { "destroyListDiag", "destroyListDefer" },
	  { "destroyListInsert" } },
	{ "caching", { "caching" }, { "dispatchJob" } },
	{ "pathfindDiag", { "pathfindDiag" },
	  { "csFindPath", "csCheckFaceConn", "findPathFull", "requestPath", "pathReqSubmit",
	    "csFindPathFallback", "contentStreamCallee_0x8869" } },
	{ "pathExtractGuard", { "pathExtractGuard" }, { "contentStreamCallee_0x8869" } },
	{ "sectionStamp", { "sectionStamp" }, { NULL } },
	{ "navMeshLife", { "navMeshLife" }, { "removeInstance" } },
	{ "sectionStamp and navMeshLife", { "sectionStamp", "navMeshLife" },
	  { "addInstance", "removeInstance" } },
	{ "gatePassDiag", { "gatePassDiag" }, { "gatesUpdateCodes", "gatesFindPath" } },
	{ "escapePauseGuard", { "escapePauseGuard" }, { "processLoading" } },
	{ "corpsePin", { "corpsePin" }, { "calculateCurrentPos" } },
#if ZONEHAND_STEP >= 2
	{ "nestValidationGuard", { "nestValidationGuard" },
	  { "finalizeZoneResources", "townListDestroy" } },
#else
	{ "nestValidationGuard", { "nestValidationGuard" }, { NULL } },
#endif
	{ "unstitchGuard", { "unstitchGuard" }, { "unstitchCrossSection" } },
	{ "graphVisitorGuard", { "graphVisitorGuard" }, { "searchSetNodeCost" } },
	{ "graphExpandGuard", { "graphExpandGuard" }, { "graphExpandNode" } },
	{ "meshFaceGuard", { "meshFaceGuard" }, { "navMeshFaceAabb" } },
	{ "unstitchProbe", { "unstitchProbe" }, { "deleteInstance" } },
	{ "sectionKeyProbe", { "sectionKeyProbe" }, { "clearanceResetKeys", "sectionCutLookup" } },
	{ "graphHeuristicGuard", { "graphHeuristicGuard" }, { "graphHeuristicGoalAdjacent", "graphHeuristicClusterCentre", "graphHeuristicCoarseSeed" } },
};

static void CheckWantTruthTable()
{
	const HookWantInputs base = DevDefaults();
	for (int f = 0; f < (int)(sizeof(kFlips) / sizeof(kFlips[0])); ++f)
	{
		const Flip& flip = kFlips[f];
		HookWantInputs in = base;
		bool ok = true;
		for (int k = 0; k < 2 && flip.fields[k]; ++k)
		{
			int j = FieldIndex(flip.fields[k]);
			ok = ok && j >= 0;
			if (j >= 0)
				in.*kFields[j].field = !(in.*kFields[j].field);
		}

		int expected = 0;
		while (expected < 8 && flip.rows[expected])
			++expected;
		int changed = 0;
		for (int i = 0; i < kDevCount; ++i)
		{
			if (HookWantEval(kDevRows[i].want, base) == HookWantEval(kDevRows[i].want, in))
				continue;
			++changed;
			bool listed = false;
			for (int e = 0; e < expected; ++e)
				if (strcmp(flip.rows[e], kDevRows[i].name) == 0)
					listed = true;
			ok = ok && listed;
		}
		ok = ok && changed == expected;
		Check(ok, Msg1("want flip %s", flip.label));
	}
}

// a matches b in every field, except that field `except` (or none, at -1) is
// b's opposite.
static bool SameInputs(const HookWantInputs& a, const HookWantInputs& b, int except)
{
	for (int j = 0; j < kFieldCount; ++j)
	{
		bool want = (j == except) ? !(b.*kFields[j].field) : (b.*kFields[j].field);
		if ((a.*kFields[j].field) != want)
			return false;
	}
	return true;
}

static void CheckInputMapping()
{
	const HookWantInputs base = HOOK_MANIFEST_UNITS_DEV_BUILD ? DevDefaults() : ProdDefaults();
	Check(SameInputs(HookWantInputsFromConfig(), base, -1), "inputs defaults");
	Check((kFields[FieldIndex("unstitchProbe")].global != NULL) == (HOOK_MANIFEST_UNITS_DEV_BUILD != 0)
		&& (kFields[FieldIndex("sectionKeyProbe")].global != NULL) == (HOOK_MANIFEST_UNITS_DEV_BUILD != 0),
		"inputs probe globals");
	for (int i = 0; i < kFieldCount; ++i)
	{
		if (!kFields[i].global)
			continue;
		bool saved = *kFields[i].global;
		*kFields[i].global = !saved;
		HookWantInputs in = HookWantInputsFromConfig();
		*kFields[i].global = saved;
		Check(SameInputs(in, base, i), Msg1("inputs %s", kFields[i].name));
	}

	// graphHeuristicGuard is an int key, so the loop above cannot flip it.
	const int savedHeuristic = fixes::g_fixesCfg.graphHeuristicGuardOn;
	fixes::g_fixesCfg.graphHeuristicGuardOn = 1;
	HookWantInputs heuristic = HookWantInputsFromConfig();
	fixes::g_fixesCfg.graphHeuristicGuardOn = savedHeuristic;
	Check(SameInputs(heuristic, base, FieldIndex("graphHeuristicGuard")), "inputs graphHeuristicGuard");
}

// The rows carrying HOOK_CAP_WORKER_POOL are exactly navMeshStop and
// buildCollision, the two hooks the worker pool requires.
static bool PoolRowsExact(const TestRow* rows, int n)
{
	int count = 0;
	bool stop = false, build = false;
	for (int i = 0; i < n; ++i)
	{
		if ((rows[i].caps & HOOK_CAP_WORKER_POOL) == 0)
			continue;
		++count;
		if (strcmp(rows[i].name, "navMeshStop") == 0)
			stop = true;
		else if (strcmp(rows[i].name, "buildCollision") == 0)
			build = true;
	}
	return count == 2 && stop && build;
}

static void CheckInstallAdmit()
{
	Check(HookInstallAdmit(true, true, true) == HOOK_ADMIT, "a re-verified row that matches is admitted");
	Check(HookInstallAdmit(true, true, false) == HOOK_ADMIT, "a re-verify decides a re-verified row whatever the gate said");
	Check(HookInstallAdmit(true, false, true) == HOOK_REFUSE_PROLOGUE, "a re-verified row that no longer matches is refused");
	Check(HookInstallAdmit(true, false, false) == HOOK_REFUSE_PROLOGUE, "a re-verified row that never matched is refused");
	Check(HookInstallAdmit(false, false, true) == HOOK_ADMIT, "a row the gate passed is admitted without a re-verify");
	Check(HookInstallAdmit(false, false, false) == HOOK_REFUSE_PROLOGUE, "a row the gate refused is never patched without a re-verify");
}

int main()
{
	CheckInstallAdmit();
#if ZONEHAND_STEP >= 3
	CheckVariant(kDevRows, kDevCount, "dev", 68, 53, 49, 25, 28, DevDefaults());
	CheckVariant(kProdRows, kProdCount, "prod", 64, 49, 44, 25, 24, ProdDefaults());
#elif ZONEHAND_STEP == 2
	CheckVariant(kDevRows, kDevCount, "dev", 67, 52, 48, 25, 27, DevDefaults());
	CheckVariant(kProdRows, kProdCount, "prod", 63, 48, 43, 25, 23, ProdDefaults());
#else
	CheckVariant(kDevRows, kDevCount, "dev", 64, 49, 45, 25, 24, DevDefaults());
	CheckVariant(kProdRows, kProdCount, "prod", 60, 45, 40, 25, 20, ProdDefaults());
#endif
	CheckDevMinusProd();
	CheckWantTruthTable();
	CheckInputMapping();
	Check(PoolRowsExact(kDevRows, kDevCount) && PoolRowsExact(kProdRows, kProdCount), "pool rows");
	return CheckExit("hook_manifest_units");
}
