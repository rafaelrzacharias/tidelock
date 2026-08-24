#pragma once
// ---------------------------------------------------------------------------------------------
// headless_test_api.h - the headless impl's test-only back door: the draw call log and a way to
//   inject events without a real window.
//
// Spec: docs/PLATFORM.md §9.4 ("readable by tests/render/present_descriptor via
//   headless_draw_log(const PlatformApi*) -> Span<const DrawCall>"; "Tests inject events by
//   pushing into the ring directly").
// Purpose: the only way anything outside impl_headless/ observes the draw call log or injects a
//   RawEvent - both are otherwise private HeadlessState fields.
// Invariants: both accessors TL_FATAL if `api` was not built by platform_headless_init (a debug
//   footgun guard, not a contract any caller should rely on triggering).
// Determinism: test-only - never included from anything but tests/ and impl_headless/ itself.
// Threading: none beyond what the caller already assumes about single-threaded test bodies.
// Includes: platform/platform.h, foundation/{ring,rect}.h, foundation/span.h.
// ---------------------------------------------------------------------------------------------
#include "platform/platform.h"
#include "foundation/ring.h"
#include "foundation/rect.h"
#include "foundation/array.h"   // Span lives with Array (containers lane; the span.h stopgap is gone)

// docs/PLATFORM.md §9.4: the call-log record. 32 B, matches the sdl3 spec's DrawCall shape.
enum HeadlessDrawVerb : u8 {
    DRAW_VERB_TEX_CREATE = 0, DRAW_VERB_TEX_UPLOAD, DRAW_VERB_TEX_LOCK, DRAW_VERB_TEX_UNLOCK,
    DRAW_VERB_TEX_DESTROY, DRAW_VERB_SET_TARGET, DRAW_VERB_SET_CLIP, DRAW_VERB_CLEAR,
    DRAW_VERB_DRAW_GEOMETRY
};
struct DrawCall { u8 verb; u8 _pad0; u16 tex; u32 n, m; Rect_i32 clip; u32 rgba; };
static_assert(sizeof(DrawCall) == 32, "docs/PLATFORM.md §9.4");

// The draw call log recorded since the last present() (present() clears it). TL_FATAL if `api`
// is not a headless PlatformApi.
Span<const DrawCall> headless_draw_log(const PlatformApi* api);

// The live event ring, for a test to push directly (the headless events.pump is a no-op).
// TL_FATAL if `api` is not a headless PlatformApi.
RingBuffer<RawEvent>* headless_event_ring(const PlatformApi* api);
