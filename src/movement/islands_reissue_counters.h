// islands_reissue_counters.h - main-thread K7 telemetry declarations.
// The 36 counters have their single definitions in islands_reissue.cpp.
#ifndef KENSHI_ZONE_OPT_ISLANDS_REISSUE_COUNTERS_H
#define KENSHI_ZONE_OPT_ISLANDS_REISSUE_COUNTERS_H
namespace islands_reissue_detail {
extern long g_k7DelPark;
extern long g_k7DelReissue;
extern long g_k7CancelStop;
extern long g_k7CancelJob;
extern long g_k7CancelTask;
extern long g_k7GameDrop;
extern long g_k7ApdDrop;
extern long g_k7DropUnconcious;
extern long g_k7DropOrderHead;
extern long g_k7DropTaskSwap;
extern long g_k7DropTaskSwapNonCombat;
extern long g_k7DropTaskSwapExpired;
extern long g_k7HoldStarted;
extern long g_k7HoldResumedSend;
extern long g_k7HoldWould;
extern long g_k7RefuseHyst;
extern long g_k7RefuseSig;
extern long g_k7RefuseDest29;
extern long g_k7RefuseDestNow;
extern long g_k7RefuseCarried;
extern long g_k7RefuseInSomething;
extern long g_k7RefuseHit;
extern long g_k7RefuseEnemies;
extern long g_k7RefuseThreats;
extern long g_k7RefuseHold;
extern long g_k7RefuseNear;
extern long g_k7RefuseZones;
extern long g_k7RefuseDestReady;
extern long g_k7RefuseCooldown;
extern long g_k7RefuseBudget;
extern long g_k7DestReadyTimeout;
extern long g_k7ArrivalArmed;
extern long g_k7ArrivalSent;
extern long g_k7ArrivalExpired;
extern long g_k7ArrivalLiveSent;
extern long g_k7ArrivalResumed;
} // namespace islands_reissue_detail
#endif // KENSHI_ZONE_OPT_ISLANDS_REISSUE_COUNTERS_H
