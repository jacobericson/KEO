#pragma once

// The D3D11 state-object switch d3dStateSkip (DEV builds; PROD links stubs):
// a startup install step on the main thread, and a main-thread tick run once
// per frame.
void InstallD3dStateSkip(int* installed, int*);
void D3dStateSkipTick(double now);
