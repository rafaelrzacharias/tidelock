#pragma once
// ---------------------------------------------------------------------------------------------
// headless_apis.h - internal seam: each impl_headless/*.cpp exposes a "build my sub-table"
//   function that init.cpp assembles into one PlatformApi.
//
// Spec: docs/PLATFORM.md §9.1 (file layout), §9.4 (headless behaviour), §9.5 (init order).
// Purpose: keeps every function-pointer target `static` (internal linkage, not state -
//   LESSONS.md) inside its own .cpp, so init.cpp only wires ctx + fn-ptr tables together.
// Invariants: every returned sub-table's `ctx` is `state` (or a member of it) - never null,
//   never anything else.
// Determinism: private to impl_headless/ - never included from tests/ (headless_test_api.h is
//   the test-facing surface) or from outside platform/.
// Threading: `headless_current_thread_id()` is safe to call from any thread.
// Includes: platform/impl_headless/headless_state.h.
// ---------------------------------------------------------------------------------------------
#include "platform/impl_headless/headless_state.h"

// Returns a WindowApi bound to `s` (docs/PLATFORM.md §9.4 "window").
WindowApi headless_window_api(HeadlessState* s);
// Returns an EventApi bound to `s` (docs/PLATFORM.md §9.4 "events").
EventApi headless_events_api(HeadlessState* s);
// Returns a DrawApi bound to `s` (docs/PLATFORM.md §9.4 "draw").
DrawApi headless_draw_api(HeadlessState* s);
// Returns a FileApi bound to `s` (docs/PLATFORM.md §9.4 "file, clock, thread").
FileApi headless_file_api(HeadlessState* s);
// Returns a ClockApi bound to `s`.
ClockApi headless_clock_api(HeadlessState* s);
// Returns a ThreadApi bound to `s`.
ThreadApi headless_thread_api(HeadlessState* s);

// The calling thread's OS id, widened to u64 (Windows DWORD / pthread_t). Used to capture the
// main thread's id at init and to answer ThreadApi.is_main later.
u64 headless_current_thread_id();

// Computes s->base_path/s->pref_path once, into arena-owned storage. Called once from init.cpp.
void headless_init_paths(HeadlessState* s);
