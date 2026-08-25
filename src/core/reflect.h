#pragma once
// ---------------------------------------------------------------------------------------------
// reflect.h - FieldKind, FieldInfo, ComponentInfo, and the one-field-list-three-doors macros:
//   TL_COMPONENT (ECS column), TL_POOL_ROW (pool rows, no column), TL_WIRE_STRUCT (wire format).
//
// Spec: docs/ECS.md §6 (design), §10.2 (this header); docs/CPP-SUBSET.md §9 R-2 (the three
//   doors), §7b (the macro catalogue rows). One table feeds four consumers: inspector, saves,
//   desync dumps, Luau field access (docs/ECS.md §6).
// Purpose: every reflected struct declares ONE TL_FIELDS_Name(X, XA, XH) list; the macros expand
//   it into the POD struct, a constexpr FieldInfo table, the POD/padding static_asserts, and
//   (wire door) the little-endian write/read pair over foundation/bytes.h.
// Invariants: the kind set is CLOSED - a field of an unlisted type fails to compile (the
//   tl_field_kind_<token> constant does not exist). Kinds are TOKEN-KEYED, not type-keyed:
//   RR-5 (ruled 2026-08-25) keeps palette rows format-keyed, so pos_t/invmass_t are ONE C++
//   type and the rev-1 kind_of overload set cannot exist (two identical overloads returning two
//   kinds); the spelled token in the field list is the only place the ROW survives to compile
//   time, so the kind constant is keyed on it (docs/ECS.md §10.2 as reconciled; TODO.md E-1).
//   sizeof == sum of field sizes is asserted per struct - every pad is an explicit _padN field.
// Determinism: the reflection table hash (tl_reflect_table_hash, folded per component in
//   registration order by world.h) is part of session_fingerprint (docs/BUILD.md §5) - it folds
//   name_hash/kind/count/offset/size FIELD-WISE, never raw struct bytes (FieldInfo carries a
//   pointer). No floats anywhere; wire io is explicit little-endian through foundation/bytes.h.
// Threading: none - constexpr tables and pure functions.
// Includes: foundation/tl_types.h, tl_assert.h, hash.h (fnv1a64), bytes.h, fx_palette.h (row
//   tokens), handle.h (Entity's shape), <stddef.h> (offsetof), <string.h> (memset/memcmp).
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/hash.h"
#include "foundation/bytes.h"
#include "foundation/fx_palette.h"
#include "foundation/handle.h"
#include <stddef.h>
#include <string.h>

// Entity - the ECS id (docs/CANON.md "Types": Handle<EntityTag, 22, 10>, u32; 4M slots, 1023
// usable generations, quarantine on wrap). Declared here rather than world.h because the
// reflection kind table and any component holding an Entity field need the type below world.h.
struct EntityTag;
using Entity = Handle<EntityTag, 22, 10>;
static_assert(sizeof(Entity) == 4, "docs/CANON.md: Entity is u32");

// The dense per-world component id (docs/CANON.md "Types": u16, < MAX_COMPONENT_TYPES) and the
// event type id (a NameHash - docs/ECS.md §5). Homed here with the rest of the reflection types.
using ComponentId = u16;
using EventTypeId = NameHash;
enum : u32 { MAX_COMPONENT_TYPES = 1024 };   // docs/CANON.md "Sizes and caps"; docs/ECS.md §9 R-1

// The core module's ErrCode range is 0x03xx (mem 0x01xx, jobs 0x02xx, net 0x04xx, alloy 0x0Axx).
// Truncation surfaces as the byte pair's own ERR_BYTES_TRUNCATED; these are the format's codes.
constexpr ErrCode ERR_WIRE_PAD_NONZERO = (ErrCode)0x0301;  // a _padN field read back nonzero

// Log-side name for a core reflect/wire ErrCode; "ERR_?" outside this header's codes.
constexpr const char* err_wire_name(ErrCode e) {
    return e == ERR_OK ? "ERR_OK"
         : e == ERR_BYTES_TRUNCATED ? "ERR_BYTES_TRUNCATED"
         : e == ERR_WIRE_PAD_NONZERO ? "ERR_WIRE_PAD_NONZERO" : "ERR_?";
}

// The closed kind enum (docs/ECS.md §10.2): scalars, the fx palette rows, the handle domains,
// StrId. A field of any other type fails to compile at its tl_field_kind_<token> lookup.
enum FieldKind : u8 {
    K_i8, K_u8, K_i16, K_u16, K_i32, K_u32, K_i64, K_u64, K_bool,
    K_pos, K_vel, K_invmass, K_stiff, K_q, K_angle, K_omega, K_dt, K_scalar,   // palette rows
    K_Entity, K_Tex, K_Font, K_Audio, K_Clip, K_Data,                          // engine handles
    K_Body, K_Constraint, K_Agent, K_Plant, K_Cavity, K_Basin,                 // Alloy handles
    K_StrId,
    K_COUNT,
};

// One reflected field (docs/ECS.md §10.2). `size` is the field's TOTAL bytes (scalar * count);
// the scalar's own width is kind_scalar_size(kind). `name` points at a literal or interned copy.
struct FieldInfo {
    const char* name;
    NameHash    name_hash;
    FieldKind   kind;
    u8          _pad0;
    u16         count;    // 1, or the array length
    u32         offset;
    u32         size;     // scalar size * count
};

// One reflected struct (docs/ECS.md §10.2). flags: COMP_SINGLETON / COMP_HIDDEN.
// `default_row` (added over the rev-1 struct, W2 ecs): a full default INSTANCE, or null for
// all-zeros. Luau-declared components build it from their per-field `default` ints
// (docs/LUAU-LAYER.md §10.6); the save decoder applies it before overlaying stored fields
// (docs/ASSETS-AND-DATA.md §5 "declared defaults for added fields"). The macros leave it null
// (C++ components default to zero); it is NOT part of the reflection hash (layout only).
struct ComponentInfo {
    const char*      name;
    NameHash         name_hash;
    u32              size;
    u32              align;
    const FieldInfo* fields;
    u32              field_count;
    u32              flags;
    const void*      default_row;
};

// ComponentInfo.flags (docs/ECS.md §2 singletons, §10.2).
enum : u32 {
    COMP_SINGLETON = 1u << 0,   // one instance in its own registered arena, world_singleton<T>
    COMP_HIDDEN    = 1u << 1,   // not shown by the generic inspector (docs/TOOLING.md §2)
};

// The scalar byte width of a kind: fx rows are i32, engine/resource handles u16, Alloy handles
// and Entity u32, StrId u16 (docs/CANON.md "Types"). Natural alignment == this width for every
// kind (docs/LUAU-LAYER.md §10.6), so there is no separate align table.
constexpr u32 kind_scalar_size(FieldKind k) {
    return (k == K_i8 || k == K_u8 || k == K_bool) ? 1u
         : (k == K_i16 || k == K_u16 || k == K_StrId
            || k == K_Tex || k == K_Font || k == K_Audio || k == K_Clip || k == K_Data) ? 2u
         : (k == K_i64 || k == K_u64) ? 8u
         : 4u;   // i32/u32, every fx row, Entity, every Alloy handle domain
}

// Element count of a constexpr array (the field tables) - deduced, never hand-counted.
template <typename T, u32 N>
constexpr u32 tl_count(const T (&)[N]) { return N; }

// Sum of the table's field sizes; == sizeof(struct) iff every pad is an explicit _padN field.
template <u32 N>
constexpr u64 tl_fields_sum_size(const FieldInfo (&f)[N]) {
    u64 s = 0;
    for (u32 i = 0; i < N; ++i) { s += f[i].size; }
    return s;
}

// --- the token-keyed kind constants (docs/ECS.md §10.2 as reconciled; TODO.md E-1) ------------
// One constexpr constant per legal field-type SPELLING. The X-macro pastes the spelled token
// (tl_field_kind_##T), so `X(pos_t, x)` and `X(invmass_t, im)` resolve to different kinds even
// though RR-5 makes them one C++ type - and an unlisted spelling is a compile error naming the
// missing constant. Field lists must spell the canonical row name (`pos_t`, never `fx<i32,18>`,
// whose comma an X-macro argument cannot carry anyway). Owners of the not-yet-built handle
// domains (Tex/Font/Audio/Clip/Data - assets; Body..Basin - Alloy) add their constants beside
// their type definitions; the enum rows above already exist for them.
constexpr FieldKind tl_field_kind_i8     = K_i8;
constexpr FieldKind tl_field_kind_u8     = K_u8;
constexpr FieldKind tl_field_kind_i16    = K_i16;
constexpr FieldKind tl_field_kind_u16    = K_u16;
constexpr FieldKind tl_field_kind_i32    = K_i32;
constexpr FieldKind tl_field_kind_u32    = K_u32;
constexpr FieldKind tl_field_kind_i64    = K_i64;
constexpr FieldKind tl_field_kind_u64    = K_u64;
constexpr FieldKind tl_field_kind_bool   = K_bool;
constexpr FieldKind tl_field_kind_pos_t     = K_pos;
constexpr FieldKind tl_field_kind_vel_t     = K_vel;
constexpr FieldKind tl_field_kind_invmass_t = K_invmass;
constexpr FieldKind tl_field_kind_stiff_t   = K_stiff;
constexpr FieldKind tl_field_kind_q_t       = K_q;
constexpr FieldKind tl_field_kind_angle_t   = K_angle;
constexpr FieldKind tl_field_kind_omega_t   = K_omega;
constexpr FieldKind tl_field_kind_dt_t      = K_dt;
constexpr FieldKind tl_field_kind_scalar_t  = K_scalar;
constexpr FieldKind tl_field_kind_lambda_t  = K_scalar;   // the alias row shares scalar_t's kind
constexpr FieldKind tl_field_kind_Entity = K_Entity;
constexpr FieldKind tl_field_kind_StrId  = K_StrId;

// --- the X-macros one TL_FIELDS_Name list is expanded through --------------------------------
// Struct-body door: X = scalar field, XA = fixed array, XH = handle field (same expansion as X;
// the separate hook is what lets the inspector treat handles specially - docs/ECS.md §10.2).
#define TL_X_FIELD(T, n)     T n;
#define TL_X_ARRAY(T, n, N)  T n[N];
#define TL_X_HANDLE(T, n)    T n;
// Field-table door: one FieldInfo row per field. TL_SELF is the alias the expanding table
// struct declares (docs/ECS.md §10.2 as reconciled - a macro cannot emit #define, so the alias
// lives in Name##_tbl's scope instead of being "defined/undefined around the expansion").
#define TL_X_INFO(T, n)      { #n, fnv1a64(#n, sizeof(#n) - 1), tl_field_kind_##T, 0, 1, (u32)offsetof(TL_SELF, n), (u32)sizeof(T) },
#define TL_X_INFO_A(T, n, N) { #n, fnv1a64(#n, sizeof(#n) - 1), tl_field_kind_##T, 0, (u16)(N), (u32)offsetof(TL_SELF, n), (u32)(sizeof(T) * (N)) },
// Wire-offset door (TL_WIRE_STRUCT only): pins each field's offset to the number the wire doc
// states, inside Name##_tbl where TL_SELF is visible.
#define TL_X_WIRE_OFFSET(n, off) static_assert(offsetof(TL_SELF, n) == (off), "wire offset drifted from the spec's number");

// The wire door's field-0 row (a named constant so the macro can splice it into the table
// without a braced initializer crossing a macro-argument boundary - braces do not protect
// commas the way parentheses do).
inline constexpr FieldInfo TL_WIRE_FV_ROW =
    { "format_version", fnv1a64("format_version", sizeof("format_version") - 1), K_u32, 0, 1, 0, 4 };

// TL_COMPONENT(Name): the ECS-column door (docs/ECS.md §6, §10.2) - the POD struct, the
// constexpr field table (TL_SELF is the alias the table struct declares: a macro cannot emit
// #define, so the alias lives in Name##_tbl's scope instead of being "defined/undefined around
// the expansion"), the POD + explicit-padding asserts, and the typed-API hook world.h's
// world_add<T>/world_get<T>/world_column<T> resolve T's info through.
// TL_COMPONENT_FLAGS(Name, FLAGS) is the same door with ComponentInfo.flags set: COMP_SINGLETON
// components declare through it (docs/ECS.md section 2, section 10.2 - flags carries
// SINGLETON/HIDDEN, and world_register_component reads info->flags).
#define TL_COMPONENT(Name) TL_COMPONENT_FLAGS(Name, 0u)
#define TL_COMPONENT_FLAGS(Name, FLAGS)                                                         \
    struct Name { TL_FIELDS_##Name(TL_X_FIELD, TL_X_ARRAY, TL_X_HANDLE) };                      \
    static_assert(__is_trivially_copyable(Name), "reflected structs are POD (docs/ECS.md section 2)"); \
    struct Name##_tbl {                                                                         \
        using TL_SELF = Name;                                                                   \
        static constexpr FieldInfo rows[] = { TL_FIELDS_##Name(TL_X_INFO, TL_X_INFO_A, TL_X_INFO) }; \
    };                                                                                          \
    static_assert(tl_fields_sum_size(Name##_tbl::rows) == sizeof(Name), "explicit padding required: name every gap _padN (docs/ECS.md section 10.2)"); \
    inline constexpr const FieldInfo* Name##_fields = Name##_tbl::rows;                          \
    inline constexpr ComponentInfo Name##_info = { #Name, fnv1a64(#Name, sizeof(#Name) - 1),    \
        (u32)sizeof(Name), (u32)alignof(Name), Name##_tbl::rows, tl_count(Name##_tbl::rows), (FLAGS), nullptr }; \
    /* typed-API hook: resolves Name's ComponentInfo for the world_* function templates */      \
    constexpr const ComponentInfo* tl_info_of(const Name*) { return &Name##_info; }

// TL_POOL_ROW(Name): the pool-row door - identical struct/table/asserts, no typed-API hook
// (pool rows are indexed by their pool, never by an ECS column - docs/CPP-SUBSET.md §9 R-2).
#define TL_POOL_ROW(Name)                                                                       \
    struct Name { TL_FIELDS_##Name(TL_X_FIELD, TL_X_ARRAY, TL_X_HANDLE) };                      \
    static_assert(__is_trivially_copyable(Name), "reflected structs are POD (docs/ECS.md section 2)"); \
    struct Name##_tbl {                                                                         \
        using TL_SELF = Name;                                                                   \
        static constexpr FieldInfo rows[] = { TL_FIELDS_##Name(TL_X_INFO, TL_X_INFO_A, TL_X_INFO) }; \
    };                                                                                          \
    static_assert(tl_fields_sum_size(Name##_tbl::rows) == sizeof(Name), "explicit padding required: name every gap _padN (docs/ECS.md section 10.2)"); \
    inline constexpr const FieldInfo* Name##_fields = Name##_tbl::rows;                          \
    inline constexpr ComponentInfo Name##_info = { #Name, fnv1a64(#Name, sizeof(#Name) - 1),    \
        (u32)sizeof(Name), (u32)alignof(Name), Name##_tbl::rows, tl_count(Name##_tbl::rows), 0u, nullptr };

// TL_WIRE_STRUCT(Name): the wire door (docs/CPP-SUBSET.md §9 R-2). Adds the leading
// `u32 format_version` as field 0 (struct member and table row), pins every offset from the
// parallel TL_OFFSETS_Name list, and generates the little-endian write/read pair over
// foundation/bytes.h. The reader zero-fills first, propagates the byte pair's sticky truncation
// code, and refuses a nonzero _padN (docs/NETCODE.md §20.2). format_version POLICY (refusing a
// newer one) is the caller's - the generated reader hands the decoded value back in the struct.
#define TL_WIRE_STRUCT(Name)                                                                    \
    struct Name { u32 format_version; TL_FIELDS_##Name(TL_X_FIELD, TL_X_ARRAY, TL_X_HANDLE) };  \
    static_assert(__is_trivially_copyable(Name), "reflected structs are POD (docs/ECS.md section 2)"); \
    static_assert(offsetof(Name, format_version) == 0, "format_version is field 0 (docs/CPP-SUBSET.md section 9 R-2)"); \
    struct Name##_tbl {                                                                         \
        using TL_SELF = Name;                                                                   \
        static constexpr FieldInfo rows[] = { TL_WIRE_FV_ROW, TL_FIELDS_##Name(TL_X_INFO, TL_X_INFO_A, TL_X_INFO) }; \
        TL_OFFSETS_##Name(TL_X_WIRE_OFFSET)                                                     \
    };                                                                                          \
    static_assert(tl_fields_sum_size(Name##_tbl::rows) == sizeof(Name), "explicit padding required: name every gap _padN (docs/ECS.md section 10.2)"); \
    inline constexpr const FieldInfo* Name##_fields = Name##_tbl::rows;                          \
    inline constexpr ComponentInfo Name##_info = { #Name, fnv1a64(#Name, sizeof(#Name) - 1),    \
        (u32)sizeof(Name), (u32)alignof(Name), Name##_tbl::rows, tl_count(Name##_tbl::rows), 0u, nullptr }; \
    /* wire write: every field low-byte-first through the field table, format_version first */  \
    inline void wire_write_##Name(ByteWriter* w, const Name* s) {                               \
        tl_wire_put_row(w, Name##_tbl::rows, tl_count(Name##_tbl::rows), s);                    \
    }                                                                                           \
    /* wire read: zero-fill, field-by-field LE, sticky truncation code, nonzero pads refused */ \
    inline ErrCode wire_read_##Name(ByteReader* r, Name* s) {                                   \
        return tl_wire_get_row(r, Name##_tbl::rows, tl_count(Name##_tbl::rows), s);             \
    }

// --- the row engine the wire door and the save encoder share ---------------------------------

// Writes one scalar of kind k at p, low byte first. The value is loaded through memcpy (host
// representation) and stored byte-explicit, so the stream is host-endian-independent.
inline void tl_wire_put_scalar(ByteWriter* w, FieldKind k, const void* p) {
    switch (kind_scalar_size(k)) {
        case 1: { u8 v;  memcpy(&v, p, 1); bw_put_u8(w, v);  break; }
        case 2: { u16 v; memcpy(&v, p, 2); bw_put_u16(w, v); break; }
        case 4: { u32 v; memcpy(&v, p, 4); bw_put_u32(w, v); break; }
        default: { u64 v; memcpy(&v, p, 8); bw_put_u64(w, v); break; }
    }
}

// Reads one scalar of kind k into p (low byte first); on underflow the byte pair's sticky code
// is set and p receives 0.
inline void tl_wire_get_scalar(ByteReader* r, FieldKind k, void* p) {
    switch (kind_scalar_size(k)) {
        case 1: { u8 v = br_get_u8(r);   memcpy(p, &v, 1); break; }
        case 2: { u16 v = br_get_u16(r); memcpy(p, &v, 2); break; }
        case 4: { u32 v = br_get_u32(r); memcpy(p, &v, 4); break; }
        default: { u64 v = br_get_u64(r); memcpy(p, &v, 8); break; }
    }
}

// Writes one row's fields in table order, each element little-endian (arrays element-wise).
inline void tl_wire_put_row(ByteWriter* w, const FieldInfo* f, u32 field_count, const void* row) {
    const u8* base = (const u8*)row;
    for (u32 i = 0; i < field_count; ++i) {
        const u32 scalar = kind_scalar_size(f[i].kind);
        for (u32 e = 0; e < f[i].count; ++e) {
            tl_wire_put_scalar(w, f[i].kind, base + f[i].offset + (u64)e * scalar);
        }
    }
}

// True iff the field is an explicit padding field (its name starts "_pad" - the docs/ECS.md
// §10.2 naming rule); pads must read back zero on the wire (docs/NETCODE.md §20.2).
inline bool tl_field_is_pad(const FieldInfo* f) {
    return f->name[0] == '_' && f->name[1] == 'p' && f->name[2] == 'a' && f->name[3] == 'd';
}

// Reads one row: zero-fills the struct, decodes fields in table order, returns the sticky
// truncation code if the input ran short, ERR_WIRE_PAD_NONZERO if an explicit pad decoded
// nonzero, else ERR_OK. On any error the out-struct holds only decoded-or-zero bytes.
inline ErrCode tl_wire_get_row(ByteReader* r, const FieldInfo* f, u32 field_count, void* row) {
    u8* base = (u8*)row;
    u64 total = 0;
    for (u32 i = 0; i < field_count; ++i) { total += f[i].size; }
    // Sum of table sizes == sizeof(struct) by the declaration-site assert, so this zero-fill
    // covers the whole row (explicit pads included) before any partial decode can return.
    memset(base, 0, (usize)total);
    for (u32 i = 0; i < field_count; ++i) {
        const u32 scalar = kind_scalar_size(f[i].kind);
        for (u32 e = 0; e < f[i].count; ++e) {
            tl_wire_get_scalar(r, f[i].kind, base + f[i].offset + (u64)e * scalar);
        }
    }
    if (!br_ok(r)) { return r->err; }
    for (u32 i = 0; i < field_count; ++i) {
        if (!tl_field_is_pad(&f[i])) { continue; }
        const u8* p = base + f[i].offset;
        for (u32 b = 0; b < f[i].size; ++b) {
            if (p[b] != 0u) { return ERR_WIRE_PAD_NONZERO; }
        }
    }
    return ERR_OK;
}

// One differing element between two rows of one component (the desync dump's currency -
// docs/ECS.md §6 consumer 3, docs/DETERMINISM.md §7 step 3). Values are the element's raw bits
// zero-extended to u64; the printer formats by field->kind.
struct DiffLine {
    const FieldInfo* field;
    u32 element;    // 0 for scalars; the array index otherwise
    u32 _pad0;
    u64 a_bits;
    u64 b_bits;
};

// Element-by-element diff of two rows of `info`'s component: appends one DiffLine per differing
// element (array fields element-wise), up to max_lines; returns the number of differing
// elements FOUND (which may exceed max_lines - the caller prints "and N more"). Defined in
// core/diff.cpp. Pure; a == b bytes give 0.
u32 reflect_diff_rows(const ComponentInfo* info, const void* a, const void* b,
                      DiffLine* out, u32 max_lines);

// The per-component reflection hash (docs/ECS.md §10.2): tl_hash64 over (name_hash, size, align,
// then per field: name_hash, kind, count, offset, size) - folded FIELD-WISE into a flat little-
// endian buffer, never over struct bytes (FieldInfo carries a pointer). world.h folds these per
// component in registration order into the session_fingerprint input (docs/BUILD.md §5).
inline u64 tl_reflect_component_hash(const ComponentInfo* info) {
    // 3 u64 header words + 5 u64 words per field, LE-encoded on scratch-free stack chunks.
    u64 h = TL_HASH_SEED;
    u8 buf[8 * 8];
    ByteWriter w;
    bw_init(&w, buf, sizeof(buf));
    bw_put_u64(&w, info->name_hash);
    bw_put_u64(&w, info->size);
    bw_put_u64(&w, info->align);
    h = tl_hash64(buf, w.len, h);
    for (u32 i = 0; i < info->field_count; ++i) {
        const FieldInfo* f = &info->fields[i];
        bw_init(&w, buf, sizeof(buf));
        bw_put_u64(&w, f->name_hash);
        bw_put_u64(&w, (u64)f->kind);
        bw_put_u64(&w, (u64)f->count);
        bw_put_u64(&w, (u64)f->offset);
        bw_put_u64(&w, (u64)f->size);
        h = tl_hash64(buf, w.len, h);
    }
    return h;
}
