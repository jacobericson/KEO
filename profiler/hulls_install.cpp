// hulls_install.cpp - Hull diagnostic site verification and installation.
// Runs during plugin initialization; takes no hull diagnostic lock.

#include "hulls_detail.h"

namespace audit {
namespace audithulls_detail {

// ---- Sites -----------------------------------------------------------------

// First 16 bytes of each target (IDA, kenshi_x64.exe Steam 1.0.65).
const unsigned char PRO_PUSH_HULL[16]   = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0xA7,0x6E,0xC6 };
const unsigned char PRO_PUSH_ENTITY[16] = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x51,0x20,0x48,0x8B };
const unsigned char PRO_PUSH_SCYTHE[16] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48 };
const unsigned char PRO_PUSH_ROOT[16]   = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0x07,0x70,0x95 };
const unsigned char PRO_PUSH_BASE[16]   = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x1D,0xA7,0x6F,0x95 };
const unsigned char PRO_ZONE_RELEASE[16] = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 };
const unsigned char PRO_DTOR_HULL[16]   = { 0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x83,0xEC,0x30,0x48,0xC7,0x44,0x24,0x20,0xFE };
// The SimplePhysXEntity-family and Scythe deleting destructors share it.
const unsigned char PRO_DTOR_SHORT[16]  = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0xDA,0x48,0x8B,0xF9,0xE8 };
const unsigned char PRO_JUNK_PRE_BT[16] = { 0x48,0x8B,0xC4,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0xD0 };

struct Site
{
	const char*          name;
	size_t               rva;
	const unsigned char* prologue;
	void*                detour;
	void**               orig;
};


} // namespace
using namespace audithulls_detail;

const char* Hulls_Install(uintptr_t exeBase, uintptr_t exeEnd, uintptr_t gameWorld, bool physUTHooked,
                          const std::string& auditDir, const std::string& runName)
{
	g_exeBase     = exeBase;
	g_exeEnd      = exeEnd;
	g_gameWorld   = gameWorld;
	g_consumerRet = exeBase + RVA_CONSUMER_RET;

	if (!physUTHooked)
	{
		AuditLine("[Audit] hullDiag refused: physUT is not hooked (the flush scan runs in it)");
		return "refused";
	}

	const Site sites[] =
	{
		// vtable slot 1 of PhysicsHullT / SimplePhysXEntity, StaticBox, StaticCapsule,
		// DoorPhysX / ScythePhysicsT / _ScytheRootObjectInterfaceT, ScytheRagdoll /
		// PhysicsThreadedBaseInterface.
		{ "hullPushHull",    0x4CC210, PRO_PUSH_HULL,   (void*)PUSH_DETOURS[EV_PUSH_HULL],   (void**)&oPush[EV_PUSH_HULL] },
		{ "hullPushEntity",  0x7DC170, PRO_PUSH_ENTITY, (void*)PUSH_DETOURS[EV_PUSH_ENTITY], (void**)&oPush[EV_PUSH_ENTITY] },
		{ "hullPushScythe",  0x7DBF20, PRO_PUSH_SCYTHE, (void*)PUSH_DETOURS[EV_PUSH_SCYTHE], (void**)&oPush[EV_PUSH_SCYTHE] },
		{ "hullPushRoot",    0x7DC0B0, PRO_PUSH_ROOT,   (void*)PUSH_DETOURS[EV_PUSH_ROOT],   (void**)&oPush[EV_PUSH_ROOT] },
		{ "hullPushBase",    0x7DC110, PRO_PUSH_BASE,   (void*)PUSH_DETOURS[EV_PUSH_BASE],   (void**)&oPush[EV_PUSH_BASE] },
		{ "hullZoneRelease", RVA_ZONE_RELEASE, PRO_ZONE_RELEASE, (void*)&ReleaseDetour, (void**)&oRelease },
		// vtable slot 0 (deleting destructor) of each class queued above.
		{ "hullDtorHull",    0x4CEBB0, PRO_DTOR_HULL,   (void*)DTOR_DETOURS[0], (void**)&oDtor[0] },
		{ "hullDtorSimple",  0x4D0390, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[1], (void**)&oDtor[1] },
		{ "hullDtorBox",     0x0EDF70, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[2], (void**)&oDtor[2] },
		{ "hullDtorCapsule", 0x4D0450, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[3], (void**)&oDtor[3] },
		{ "hullDtorDoor",    0x4D0510, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[4], (void**)&oDtor[4] },
		{ "hullDtorScythe",  0x7E16F0, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[5], (void**)&oDtor[5] },
		{ "hullDtorRoot",    0x7E0BA0, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[6], (void**)&oDtor[6] },
		{ "hullDtorRagdoll", 0x7E9A70, PRO_DTOR_SHORT,  (void*)DTOR_DETOURS[7], (void**)&oDtor[7] },
		{ "hullJunkPreBT",   RVA_JUNK_PRE_BT, PRO_JUNK_PRE_BT, (void*)&JunkDetour, (void**)&oJunk },
	};
	const int n = (int)(sizeof(sites) / sizeof(sites[0]));

	for (int i = 0; i < n; ++i)
	{
		std::string check = AuditCheckExe(sites[i].name, sites[i].rva, sites[i].prologue);
		if (check != "ok" && check != "shared")
		{
			AuditLine(Fmt("[Audit] hullDiag refused: %s ", sites[i].name) + check);
			return "refused";
		}
	}

	Slot* slots = (Slot*)VirtualAlloc(NULL, TABLE_SLOTS * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!slots)
	{
		AuditLine("[Audit] hullDiag refused: table allocation failed");
		return "refused";
	}
	g_table.Init(slots, TABLE_SLOTS);

	int ok = 0;
	for (int i = 0; i < n; ++i)
		if (AuditHookExe(sites[i].name, sites[i].rva, sites[i].prologue, sites[i].detour, sites[i].orig))
			++ok;
	if (ok != n)
	{
		AuditLine(Fmt("[Audit] hullDiag off: %d/%d hooks installed (the rest pass through)", ok, n));
		return "off";
	}
	CreateDirectoryA(auditDir.c_str(), NULL);
	std::string crashPath = auditDir + runName + "_hullcrash.txt";
	g_crashFile = CreateFileA(crashPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
	                          FILE_ATTRIBUTE_NORMAL, NULL);
	g_veh = g_crashFile != INVALID_HANDLE_VALUE ? AddVectoredExceptionHandler(0, &HullFaultHandler) : NULL;

	g_lastReport = Now();
	InterlockedExchange(&g_on, 1);
	AuditLine(Fmt("[Audit] hullDiag on: %d hooks, table %u slots, ring %I64d, fault record %s", n, TABLE_SLOTS,
	              RING, g_veh ? crashPath.c_str() : "off (file or handler failed)"));
	return "on";
}

} // namespace audit
