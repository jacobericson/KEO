#pragma once

// What the Benchmark section's buttons do. Main thread only, from a button
// press while Options is open.

// id is BENCH_BUTTON_RECORD or BENCH_BUTTON_RUN plus the slot, or
// BENCH_BUTTON_SWEEP; speed is the slot's staged speed choice (1 or 20). While
// a sweep or a run is active every button aborts it instead. Returns the slot
// whose run is active afterwards, or -1 (an abort takes effect on the
// runner's next tick, so it returns -1).
int BenchButtonPressed(int id, int speed);
