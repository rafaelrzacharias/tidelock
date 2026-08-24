// draw.cpp - the headless DrawApi: a validating stub (docs/PLATFORM.md §9.4 "draw"). Every
// argument check the sdl3 impl makes (§9.3 "draw"), no device; streaming textures get a real CPU
// buffer so tests can read back what they wrote.
#include "platform/impl_headless/headless_apis.h"
#include "platform/impl_headless/headless_test_api.h"

#include "foundation/tl_assert.h"

namespace {

void log_call(HeadlessState* s, u8 verb, TexHandle tex, u32 n, u32 m, u32 rgba) {
    DrawCall dc;
    dc.verb = verb;
    dc._pad0 = 0u;
    dc.tex = (u16)handle_index(tex);
    dc.n = n;
    dc.m = m;
    dc.clip = s->has_clip ? s->clip : Rect_i32{ 0, 0, 0, 0 };
    dc.rgba = rgba;
    (void)ring_push(&s->draw_log, dc);   // overwrite_oldest, so this never fails
}

// Live rec for `h`, or null with `*err` set to TEX_STALE (covers null, out-of-range, dead and
// generation-mismatched handles alike - one code, matching docs/PLATFORM.md §9.3's "stale handle").
HeadlessTexRec* lookup_tex(HeadlessState* s, TexHandle h, ErrCode* err) {
    *err = (ErrCode)ERR_PLATFORM_TEX_STALE;
    if (handle_is_null(h)) { return nullptr; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_TEX || !s->tex[idx].alive || handle_gen(h) != s->tex_gen[idx]) {
        return nullptr;
    }
    *err = ERR_OK;
    return &s->tex[idx];
}

Result<TexHandle> hd_texture_create(void* ctx, u16 w, u16 h, u8 fmt, u8 usage) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (w == 0u || h == 0u || w > 8192u) {
        return Result<TexHandle>{ TexHandle{}, (ErrCode)ERR_PLATFORM_TEX_BAD_ARG };
    }
    u32 idx = HEADLESS_MAX_TEX;
    for (u32 i = 0; i < HEADLESS_MAX_TEX; ++i) {
        if (!s->tex[i].alive) { idx = i; break; }
    }
    if (idx == HEADLESS_MAX_TEX) {
        return Result<TexHandle>{ TexHandle{}, (ErrCode)ERR_PLATFORM_TEX_LIMIT };
    }
    if (s->tex_gen[idx] == 0u) { s->tex_gen[idx] = 1u; }   // first issue of this slot ever
    HeadlessTexRec& rec = s->tex[idx];
    rec.alive = 1u; rec.usage = usage; rec.fmt = fmt; rec._pad0 = 0u; rec.w = w; rec.h = h;
    if (usage == TEX_STREAMING) {
        rec.pitch = (u32)w * 4u;   // PIXFMT_RGBA8
        rec.streaming_buf = (u8*)arena_push(&s->arena, (u64)rec.pitch * (u64)h, 16u);
    } else {
        rec.pitch = 0u; rec.streaming_buf = nullptr;
    }
    ++s->tex_live_count;
    const TexHandle out = handle_make<TexHandle>(idx, (u32)s->tex_gen[idx]);
    log_call(s, DRAW_VERB_TEX_CREATE, out, w, h, 0u);
    return Result<TexHandle>{ out, ERR_OK };
}

ErrCode hd_texture_upload(void* ctx, TexHandle h, const void*, u32 pitch) {
    HeadlessState* s = (HeadlessState*)ctx;
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (rec == nullptr) { return err; }
    if (rec->usage != TEX_STATIC) { return (ErrCode)ERR_PLATFORM_TEX_USAGE; }
    log_call(s, DRAW_VERB_TEX_UPLOAD, h, pitch, 0u, 0u);
    return ERR_OK;
}

Result<u8*> hd_texture_lock(void* ctx, TexHandle h, u32* pitch_out) {
    HeadlessState* s = (HeadlessState*)ctx;
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (rec == nullptr) { return Result<u8*>{ nullptr, err }; }
    if (rec->usage != TEX_STREAMING) { return Result<u8*>{ nullptr, (ErrCode)ERR_PLATFORM_TEX_USAGE }; }
    if (pitch_out != nullptr) { *pitch_out = rec->pitch; }
    log_call(s, DRAW_VERB_TEX_LOCK, h, 0u, 0u, 0u);
    return Result<u8*>{ rec->streaming_buf, ERR_OK };
}

// The one verb in this table that validated NOTHING: it logged a TEX_UNLOCK for any handle,
// including a null or destroyed one, and never checked the usage. Section 9.4 says the headless
// draw stub makes "every argument check as sdl3" - and unlock is void, so there is no error code
// to return: a bad unlock is a caller bug, which is what TL_FATAL is for (docs/CPP-SUBSET.md
// section 3). Its two failure modes are exactly the two `lock` already refuses with TEX_STALE and
// TEX_USAGE.
void hd_texture_unlock(void* ctx, TexHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (rec == nullptr) {
        TL_FATAL("DrawApi.texture_unlock on a null or stale TexHandle (docs/PLATFORM.md section 9.4)");
    }
    if (rec->usage != TEX_STREAMING) {
        TL_FATAL("DrawApi.texture_unlock on a non-streaming texture - only lock/unlock streaming (docs/PLATFORM.md section 9.3)");
    }
    log_call(s, DRAW_VERB_TEX_UNLOCK, h, 0u, 0u, 0u);
}

// Unlike unlock, a stale handle here answers 0x0 rather than dying: `texture_size` is the verb a
// caller uses to ASK about a handle it may not trust, and 0x0 is an answer, not a swallowed
// error. docs/PLATFORM.md section 9.4 records it; headless_draw_texture_limit_and_stale pins it.
void hd_texture_size(void* ctx, TexHandle h, u16* w_out, u16* h_out) {
    HeadlessState* s = (HeadlessState*)ctx;
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (w_out != nullptr) { *w_out = rec != nullptr ? rec->w : 0u; }
    if (h_out != nullptr) { *h_out = rec != nullptr ? rec->h : 0u; }
}

void hd_texture_destroy(void* ctx, TexHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (rec == nullptr) { return; }   // stale/null destroy is a no-op, not a crash
    rec->alive = 0u;
    // bumped NOW, so this same handle is stale even before reuse; wrap-to-1, not ++ (headless_state.h)
    s->tex_gen[handle_index(h)] = headless_gen_next<TexHandle>(s->tex_gen[handle_index(h)]);
    --s->tex_live_count;
    log_call(s, DRAW_VERB_TEX_DESTROY, h, 0u, 0u, 0u);
}

ErrCode hd_set_target(void* ctx, TexHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (handle_is_null(h)) {
        s->has_target = 0u;
        log_call(s, DRAW_VERB_SET_TARGET, h, 0u, 0u, 0u);
        return ERR_OK;
    }
    ErrCode err;
    HeadlessTexRec* rec = lookup_tex(s, h, &err);
    if (rec == nullptr) { return err; }
    if (rec->usage != TEX_TARGET) { return (ErrCode)ERR_PLATFORM_TEX_USAGE; }
    s->has_target = 1u; s->target = h;
    log_call(s, DRAW_VERB_SET_TARGET, h, 0u, 0u, 0u);
    return ERR_OK;
}

void hd_set_clip(void* ctx, const Rect_i32* r) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (r != nullptr) { s->has_clip = 1u; s->clip = *r; } else { s->has_clip = 0u; }
    log_call(s, DRAW_VERB_SET_CLIP, TexHandle{}, 0u, 0u, 0u);
}

void hd_clear(void* ctx, u32 rgba) {
    log_call((HeadlessState*)ctx, DRAW_VERB_CLEAR, TexHandle{}, 0u, 0u, rgba);
}

ErrCode hd_draw_geometry(void* ctx, TexHandle tex, const DrawVertex* v, u32 n, const u32* idx, u32 m) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (m % 3u != 0u) { return (ErrCode)ERR_PLATFORM_TEX_BAD_ARG; }
    if (n > 65536u) { return (ErrCode)ERR_PLATFORM_TEX_BAD_ARG; }
    if (v == nullptr && n > 0u) { return (ErrCode)ERR_PLATFORM_TEX_BAD_ARG; }
    if (idx == nullptr && m > 0u) { return (ErrCode)ERR_PLATFORM_TEX_BAD_ARG; }
    if (!handle_is_null(tex)) {
        ErrCode err;
        if (lookup_tex(s, tex, &err) == nullptr) { return err; }
    }
    log_call(s, DRAW_VERB_DRAW_GEOMETRY, tex, n, m, 0u);
    return ERR_OK;
}

void hd_present(void* ctx) {
    HeadlessState* s = (HeadlessState*)ctx;
    s->draw_log.head = 0u;
    s->draw_log.tail = 0u;   // "cleared by present" (docs/PLATFORM.md §9.4)
}

u32 hd_live_textures(void* ctx) {
    return ((HeadlessState*)ctx)->tex_live_count;
}

}  // namespace

DrawApi headless_draw_api(HeadlessState* s) {
    return DrawApi{ s, hd_texture_create, hd_texture_upload, hd_texture_lock, hd_texture_unlock,
                    hd_texture_size, hd_texture_destroy, hd_set_target, hd_set_clip, hd_clear,
                    hd_draw_geometry, hd_present, hd_live_textures };
}

Span<const DrawCall> headless_draw_log(const PlatformApi* api) {
    TL_CHECK(api != nullptr && api->is_headless);
    HeadlessState* s = (HeadlessState*)api->draw.ctx;
    // present() resets head/tail to 0, so a live log can only wrap the physical array if a single
    // frame pushes >= HEADLESS_DRAW_LOG_CAP calls without an intervening present() - not reached
    // by anything landing today; documented rather than handled, since Span<T> needs contiguity.
    const RingBuffer<DrawCall>& r = s->draw_log;
    return Span<const DrawCall>{ r.data + (r.tail & (r.cap - 1u)), r.head - r.tail };
}
