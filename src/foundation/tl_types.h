#pragma once
// ---------------------------------------------------------------------------------------------
// tl_types.h - the width-exact scalar vocabulary every tidelock TU speaks.
//
// Spec: docs/CPP-SUBSET.md §1, §3; names and widths are fixed by docs/CANON.md ("Types").
// Purpose: one spelling for every integer width, plus the two closed generics the subset
//   sanctions here - `uint_fit<N>` (the only type chooser in the codebase) and `Result<T>`
//   (the only recoverable-failure shape).
// Invariants: every type below is trivially copyable and has no padding; the static_asserts at
//   the foot of this header are the enforcement and run in every TU that includes it.
// Determinism: these are storage types only - no arithmetic helpers live here (wrap/sat/mul
//   belong to fx, docs/FX-PALETTE.md). `f32`/`f64` are declared for render/editor/tools/platform
//   and are banned tokens in sim TUs (tools/audit/includes.py).
// Threading: none - header-only, no state.
// Includes: <stdint.h> and <stddef.h> only; both are on the allowlist of docs/CPP-SUBSET.md §1.
// ---------------------------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

// Float is legal in render/, editor/, platform/, tools/ and tests of those; never in sim TUs.
using f32 = float;
using f64 = double;

using usize = size_t;

// --- uint_fit<N>: the smallest unsigned type holding N bits (docs/CPP-SUBSET.md §1) ----------
// Flat, non-recursive: a constexpr index function plus four explicit specialisations.
namespace tl {

// Bit count -> row of the table below. Precondition: N >= 1; N > 64 is a compile error via the
// missing specialisation.
constexpr u32 uint_fit_row(u32 bits) { return bits <= 8 ? 0u : bits <= 16 ? 1u : bits <= 32 ? 2u : 3u; }

template <u32 ROW> struct UIntRow;
template <> struct UIntRow<0> { using Type = u8; };
template <> struct UIntRow<1> { using Type = u16; };
template <> struct UIntRow<2> { using Type = u32; };
template <> struct UIntRow<3> { using Type = u64; };

}  // namespace tl

template <u32 BITS>
using uint_fit = typename tl::UIntRow<tl::uint_fit_row(BITS)>::Type;

// --- the error model (docs/CPP-SUBSET.md §3) --------------------------------------------------
// ErrCode: a closed `enum : u16` per module, 0 == OK, one range per module. This header owns
// only the width and the OK value; each module declares its own enum over them.
using ErrCode = u16;
constexpr ErrCode ERR_OK = 0;

// Result<T>: the ONE sanctioned recoverable-failure shape. `value` is UNDEFINED when `err != 0`
// and must never be read; callers test `err` first. `[[nodiscard]]` is mandatory
// (docs/CPP-SUBSET.md §9 R-1); the sanctioned "cannot fail" form is TL_CHECK(call().err == 0).
template <class T>
struct [[nodiscard]] Result {
    T value;
    ErrCode err;
};

// `Result<void>` is spelled `ErrCode` - the template is never instantiated on void.

static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8, "");
static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8, "");
static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "IEEE-754 binary32/binary64 assumed");
static_assert(sizeof(usize) == sizeof(void*), "");
static_assert((u8)~(u8)0 == 255 && (i8)-1 == ~(i8)0, "two's complement, 8-bit bytes");
static_assert(__is_same(uint_fit<1>, u8) && __is_same(uint_fit<8>, u8), "");
static_assert(__is_same(uint_fit<9>, u16) && __is_same(uint_fit<16>, u16), "");
static_assert(__is_same(uint_fit<17>, u32) && __is_same(uint_fit<32>, u32), "");
static_assert(__is_same(uint_fit<33>, u64) && __is_same(uint_fit<64>, u64), "");
static_assert(__is_trivially_copyable(Result<u32>), "Result must survive a memcpy");
static_assert(sizeof(Result<u32>) == 8 && sizeof(Result<u8>) == 4, "explicit layout, no surprises");
