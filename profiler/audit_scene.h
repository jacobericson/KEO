// audit_scene.h - The scene and render probes (SceneDetail): the main scene manager's fork inputs,
// the render-queue rows and hooks, and the D3D11 render system's state reads. Main thread only.

#ifndef KENSHI_FRAME_AUDIT_SCENE_H
#define KENSHI_FRAME_AUDIT_SCENE_H

#include "audit_detail.h"

namespace kenshiframeaudit_detail
{

// The render system's state flags at a draw's entry, and which of them would have kept their object.
struct D3dFlags
{
	unsigned char blend, raster, depth, sampler;
	unsigned char same;   // bit i: state i would have kept its bound object at entry
};

void InstallScene();                       // startup, after InstallOffMain (main thread)
const char* SceneStatus();                 // "on", "partial" or "off"
// OffMainProbeEnter / OffMainProbeExit hand every tag at or above ST_SCENE_FIRST here first.
void SceneProbeEnter(int tag, CallSiteProbe::U64 a, CallSiteProbe::U64 b);
void SceneProbeExit(int tag, CallSiteProbe::U64 ret, LONGLONG t0, LONGLONG t1);
void SceneSyncInputs(void* barrier);       // main: hk_BarrierSync, before any timing
void SceneFrameTotals(FrameRec& r);        // main, at frame close
void SceneOncePerSecond();                 // main: the [AUDIT-RQ] line every 5 s
// audit_d3d.cpp, used by audit_scene.cpp and hk_D3DRender:
extern const int NUM_D3D_SITES;                       // the D3D11 call-site rows
const char* InstallD3dProbes(HMODULE d3d, int* rows);   // NULL when all installed, else the reason
bool D3dStateOn();
void D3dStateEnter(void* rs, D3dFlags* f);
void D3dStateExit(void* rs, const D3dFlags& f);
void D3dProbeExit(int tag, LONGLONG t0, LONGLONG t1);
void D3dFrameTotals(FrameRec& r);

} // namespace kenshiframeaudit_detail

#endif
