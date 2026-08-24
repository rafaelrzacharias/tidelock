# The C++ subset — coding contract (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. Expands `PIVOT-DESIGN.md` §2 into the rules a code review
> checks and CI enforces. PIVOT §2 is the ruling; this doc is its operational form.
> **Scope:** every TU under `src/`. `vendor/` and `tools/` are exempt (they compile in their own
> TUs with their own flags and must not leak includes into ours).

---

## 0. Why a subset at all

The reasons Ore existed survive as rules: no hidden control flow, no hidden allocation, no hidden
cost, fast compiles, and state that a memcpy can snapshot. C++ gives all of those *if* you refuse
most of it. The subset is "C with namespaces, a handful of flat templates, and `static_assert`".

---

## 1. Banned (DECIDED)

| Banned in `src/` | Why | Enforcement |
|---|---|---|
| STL containers, algorithms, streams, `<string>`, `<memory>` | RAII contract vs no-destructors; implementation-defined iteration/hash order; compile weight | include firewall (CI grep on `#include <` outside the allowlist) |
| RTTI, exceptions | hidden control flow, codegen surface | `-fno-exceptions -fno-rtti` |
| inheritance, virtual, polymorphism | hidden dispatch, vtables in POD | review + `-Wnon-virtual-dtor` as a tripwire (any hit = violation) |
| destructors, RAII, copy/move ctors | POD everywhere; snapshot = memcpy | `static_assert(__is_trivially_copyable(T))` (the clang builtin — `<type_traits>` is banned) at every registration door |
| operator overloads beyond fx arithmetic | hidden cost | review |
| `new`/`delete`/`malloc`/`free` outside arena backing | zero per-tick allocation | symbol audit (§4) + debug counting shim |
| `<math.h>` in sim TUs | libm is the cross-platform determinism hole | include firewall + symbol audit |
| static mutable state | two-worlds-one-process test; rollback restores only registered arenas | link gate: every object file in every `src/` lib must have zero bytes of `.data`/`.bss`/TLS (`tools/audit/symbols.py`), **except the named tooling-plane stems (§9 R-4)** — never hashed, never snapshotted, never part of a world's registered arena set, so none of this row's reasons apply to them. A grep cannot see anonymous-namespace globals, `inline static` or static locals; the earlier `^static [^c]` line also rejected every `static` *function*, which is internal linkage, not state |
| recursive/meta templates, SFINAE, concepts, expression templates | compile time + cognitive cost | review; the sanctioned list is closed |
| `auto` for non-iterator locals, lambdas capturing by reference across a call | readability / hidden lifetime | review |
| `thread_local` outside the job system's worker slot | hidden per-thread state the hash can't see | CI grep |

**Allowed system includes in `src/`:** `<stdint.h>`, `<stddef.h>`, `<string.h>` (memcpy/memset/
memcmp/**memmove** only - `memmove` is sanctioned because `CONTAINERS.md` §8's erase/insert paths
need an overlapping move and it is as deterministic as the other three; the earlier list omitted
it and contradicted that doc), `<limits.h>`. `platform/` additionally includes its OS/SDL headers inside its own
TUs. `<math.h>` is allowed ONLY in `render/`, `editor/`, `platform/` (float is legal there). The
named tooling-plane stems (§9 R-4) additionally get `<stdio.h>`/`<stdlib.h>`/`<stdarg.h>` - still
no OS header, no `<math.h>`; the real crash writer's raw OS calls stay `platform/`'s
(`TOOLING.md` §9.3.9).
Type traits come from clang builtins, never `<type_traits>`: `__is_trivially_copyable(T)`,
`__is_same(A,B)`, `__is_enum(T)`, `alignof`, `sizeof`, `offsetof` (from `<stddef.h>`). `tl_types.h`
defines `u8..u64`, `i8..i64`, `f32`, `f64`, `usize` and `uint_fit<N>` (a `constexpr` width
selector: ≤8→u8, ≤16→u16, ≤32→u32, else u64) — the only place a "type chooser" template exists.

---

## 2. Sanctioned templates (DECIDED — closed list)

Flat value templates with enumerated instantiation sets. Adding a row here is a design decision.

| Template | Instantiation set | Doc |
|---|---|---|
| `fx<Rep, FRAC>` | the palette rows only (`fx_palette.h`) | `FX-PALETTE.md` |
| `Array<T>`, `Span<T>` | any trivially-copyable T | `CONTAINERS.md` |
| `SlotMap<T>` | per domain, with its `Handle` | `CONTAINERS.md` |
| `Map<K,V>` | integer/hashed keys, POD values | `CONTAINERS.md` |
| `SortedMap<K,V>` / `SortedSet<K>` | integer keys | `CONTAINERS.md` |
| `RingBuffer<T>` | POD T | `CONTAINERS.md` |
| `Handle<Tag, IDX_BITS, GEN_BITS>` | per domain | `MEMORY.md` §3 |
| `EventQueue<T>` | POD event types | `ECS.md` §5 |
| `Result<T>` | any T (see §3) | this doc |

`constexpr` functions and `static_assert` are encouraged without limit — they are the enforcement
tools. Function templates are allowed only as thin typed wrappers over type-erased calls
(`world_get<T>` over `world_get(id)`), never as the mechanism.

---

## 3. Error model (DECIDED — PIVOT §2)

Three axes, three shapes, nothing else:

| Axis | Shape | Notes |
|---|---|---|
| programmer bug / invariant | assert tiers: `TL_ASSERT` (debug only), `TL_CHECK` (all tiers, slim), `TL_FATAL(msg)` (all tiers) | fatal prints file:line + message, writes the crash report (`TOOLING.md` §6), aborts. Never returned, never swallowed |
| recoverable failure | `Result<T> { T value; ErrCode err; }` returned by value; `ErrCode` is a closed `enum : u16` per module, 0 = OK | the ONE sanctioned shape. Error-code out-params are banned. `value` is undefined when `err != 0` — never read it. Callers check `r.err` first; a `TL_CHECK(r.err == 0)` is the allowed "I know this can't fail" form |
| absence | null handle (`bits == 0`) or a documented-nullable `T*` | generation 0 is never issued; zeroed memory is never a valid handle |

`ErrCode`, `ERR_OK` and `Result<T>` live in `foundation/tl_types.h` (§1) — the leaf every module
already includes. `ErrCode` is one `enum [[nodiscard]] : u16 { ERR_OK = 0 }`, **not** an alias of
`u16`: R-1 makes `[[nodiscard]]` mandatory on it, and an alias cannot carry the attribute, which
would leave every `Result<void>`-shaped call silently discardable. A module spells its own codes in
its own range over that enum (`constexpr ErrCode ERR_MEM_OOM = (ErrCode)0x0101;`) — the width and
the attribute are the shared fact, the values are the module's.
`Result<void>` is spelled `ErrCode`. Errors carry no strings: a module's `ErrCode` enum has a
`constexpr` name table for the log. Fail-loud is policy: validators reject at `init()`, loads fail
with a named code, there are no partial states.

---

## 4. The symbol-audit gate — the `@deterministic` replacement (DECIDED)

All sim code (`src/sim/`, plus the det halves of `src/foundation/`: fx, det math, rng, hash,
containers, arenas) compiles into static libs. CI runs `llvm-nm --undefined-only` over each and
**fails on any undefined symbol outside an allowlist**:

```
allowed:   memcpy memset memcmp  (and our own tl_* symbols from the same lib set)
banned:    malloc free calloc realloc operator new/delete
           sin cos tan sqrt pow exp log fmod floor ceil ... (all of libm)
           time clock* gettimeofday QueryPerformanceCounter rand srand getrandom BCrypt*
           fopen fread fwrite printf fprintf socket send recv ... (all io)
```

A grep line in the same job covers what never becomes a symbol: `__rdtsc`, `rdtsc`, `asm`,
`__builtin_ia32_*`, `std::` and `thread_local` (all three spellings). **Mutable static state
is deliberately NOT in that list** - no grep sees an anonymous-namespace global, an `inline
static` member or a static local - so §1's rule is enforced by the link gate's
zero-`.data`/`.bss`/TLS check over every `src/` lib instead. The sim-lib boundary this needs is
the `ARCHITECTURE.md` §1 layout doing double duty. This is a callgraph effect ban at link
granularity — about 90% of what the attribute gave. §9 R-4 names the one exception: the tooling
plane, which this gate's `--data-only` libs still check for everything except the named stems.

---

## 5. UB discipline — the new thing that can silently break bit-exactness (DECIDED)

With no floats, two peers diverge in exactly two ways: undefined behaviour, and **language
constructs whose width, signedness or layout the target chooses**. The second half of that
sentence was missing until the W0 reviews measured it — `char`, `long`, bit-fields, unfixed enum
bases and `size_t`'s type identity all differ between windows-msvc and linux/aarch64 with no UB
anywhere. Rules:

- **Sanctioned arithmetic helpers are the only arithmetic in quanta paths:** `wrap_add/sub/mul`
  (two's-complement, explicit), `sat_add/sub/mul` (saturating), `mul_widen` (i32×i32→i64),
  `mulhi`, and the fx `mul<R>/div<R>` helpers. Plain `+ - *` on signed ints is allowed only where
  overflow is provably impossible by range (state the range in a comment or a `static_assert` on
  the palette row).
- **Shifts:** right shift of negative values is arithmetic on every supported compiler, but it is
  implementation-defined in the standard until C++20 — we compile as C++20 (§7) so it is defined.
  Left shift of negative values is UB: use `wrap_mul` by a power of two.
- **Hashed state uses explicitly-padded structs** (every pad named `_pad0`, zeroed at construction)
  and zero-filled arenas; the hash covers `[base, used)`, never capacity. Each pool's header states
  its ruling ("this pool is hashed over used rows; padding is explicit").
- **No reading uninitialized memory:** arenas hand out zeroed pages; scratch is poisoned `0xDD` in
  debug so a read shows up as garbage, and ASan/MSan runs catch it.
- **UBSan + ASan are part of the determinism CI**, not optional hygiene, and a report is a
  FAILURE: the sanitizer lane builds with `-fno-sanitize-recover=all` (UBSan's default is to
  print and continue to exit 0 — the W1 fx review found a signed overflow reported in the log of
  a green run). A G-06 / S-01 hash divergence is treated as UB until proven otherwise.
- **No pointer comparisons except equality**, no pointer-to-integer keys (ordering by address is a
  nondeterminism source — `DETERMINISM.md` §2).
- **Target-variable language constructs are banned in sim TUs.** Two gates, split by what each
  can actually see. **`tools/audit/targets.py` measures** the layout and preprocessor classes -
  it preprocesses every sim TU and dumps its record layouts for all three triples and diffs, so
  a `#pragma pack`, an `alignas`, a `[[no_unique_address]]`, a bit-field or any per-target `#if`
  is caught in *any* spelling rather than by whichever spelling someone listed. **`tools/audit/includes.py` bans the tokens** whose divergence is in the VALUE, over identical
  text and identical layouts, which no diff can see - and nothing belongs to both lists: `char` (signed on x86-64, unsigned on aarch64), `long` (32-bit on Windows,
  64-bit on Linux), `wchar_t`, `size_t`/`ptrdiff_t`/`intptr_t`/`max_align_t`, and the
  **`int_fast*`/`int_least*` families** — the last of these is the measured boundary of the
  cross-target gate: clang's freestanding `<stdint.h>` defines them as the *least* types and no
  hosted libc does (MSVC's `int_fast16_t` is `int`, glibc's is `long`), so a record holding one
  is 8 B on Windows and 16 B on Linux while all three of the gate's legs see 8. **Platform
  macros** (`_WIN32`, `_MSC_VER`, `__aarch64__`, `__LP64__`, `__GNUC__`, …) and
  **`__has_include`**, since a sim TU that branches on the target is two different programs and
  `__has_include` of a platform header answers one way in the real build and another under the
  gate's freestanding model. **Custom section attributes**, which hide storage from the
  `.data`/`.bss` gate. And **non-ASCII bytes in literals**, written directly *or as a `\x`/octal escape*, because a byte
  ≥ 0x80 hashes differently where `char` is signed — `const char*` message literals stay legal,
  which is exactly why the byte rule is needed. `usize` is `u64` rather than `size_t` for the same
  class of reason (`CANON.md`): same width everywhere, but not reliably the same *type*, and a
  `usize`-keyed template would select differently per target. **Literals that carry a banned
  type with no type token** (the W1 fx review planted them past every gate): a floating literal
  in any spelling (`1.0`, `1e3`, `0x1p3` — `decltype(1.0) x`, `auto y = 0x1p3` and `k * 2.5` are
  doubles), the extra float types `_Float16`/`__fp16`/`__bf16`/`__float128`, and an integer
  literal suffixed `L`/`UL` (a `long`; `LL`/`ULL` stay legal — `long long` is 64-bit
  everywhere). **`__DATE__`, `__TIME__` and
  `__TIMESTAMP__` are banned in all of `src/`** — two peers building one tree get different bytes.
  `__FILE__`/`__LINE__` stay legal (deterministic given the tree, and `TL_CHECK` expands them into
  every sim TU) but must never feed sim state.

  **Bit-fields, `#pragma pack`, `alignas`, `[[no_unique_address]]`, enums without a fixed
  underlying type and every other layout or macro construct are the *measurement* gate's**, not
  the token bans' — in any spelling, which is the whole point of measuring. Its stated
  boundaries: it compiles freestanding with a stubbed `<string.h>`, so the `int_fast*` family
  and `__has_include` fall to the token bans above; and it measures the TUs under `src/sim` and
  the det half of `src/foundation`, so a record instantiated only from `net`/`script`/`save` is
  covered by its own module's `TL_WIRE_STRUCT` asserts and, on aarch64, only once RR-1 makes a
  Pi build possible.

---

## 6. Naming and file discipline (DECIDED)

- `snake_case` for functions/variables/files, `PascalCase` for types, `UPPER_SNAKE` for constants
  and macros, `tl_` prefix on exported C-ABI symbols, `TL_` on macros. Namespaces: one per module
  (`fx`, `mem`, `ecs`, `alloy`, `net`, …), never nested more than one deep.
- One module per folder, `module.h` is the public header; everything else in the folder is
  private unless listed in `module.h`. Include with the module path: `#include "core/ecs.h"`.
- Soft file cap ~800 lines. Split at seams, not arbitrarily.
- Comments state constraints the code can't (ranges, hashing rulings, ordering invariants) — not
  what the next line does.
- **Documentation standard (RULED 2026-08-22; `tools/audit/includes.py` checks the first two):**
  (1) every `module.h` opens with a 10–20 line **contract block**: purpose, owning doc and section
  (`// Spec: docs/ECS.md §10.3`), invariants, determinism notes (hashed? snapshotted? tick-scoped
  pointers?), threading notes (which phase, chunk-keyed?); (2) every public function carries a
  one-line contract comment — preconditions, postconditions, the `ErrCode`s it can return, and
  whether it may run inside a tick; (3) pool/row headers state their hashing and reuse-zeroing
  ruling (`MEMORY.md` §1.1); (4) **no Doxygen** and no generated C++ API reference — the public
  headers are the reference and are kept readable for it; (5) the Luau binding reference
  (`script/docs/*.md`) is generated from the binding tables by `tools/luauc --docs`
  (`LUAU-LAYER.md` §10.9) and committed, so the game-author docs cannot drift from the bindings.
- Every public function has tests: happy + error + edges (zero/one/many, empty/full, min/max,
  null handle, malformed). Property/fuzz for parsers and math. No commit without tests.

---

## 7. Language level and flags (DECIDED)

- **C++20, clang only** (`BUILD.md` §1). Used from C++20: designated initializers, `constexpr`
  improvements, defined signed-shift semantics, `<bit>`-free bit ops (we write our own to avoid the
  header). **Not used:** modules, coroutines, concepts, ranges, `<format>`, three-way comparison.
- Flags: `-std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra -Werror
  -Wconversion -Wsign-conversion -Wshadow -Wvla -fno-strict-aliasing` (we type-pun through memcpy;
  strict aliasing buys nothing here and costs audit time). `-ffast-math` is banned everywhere, even
  render-side (keeps one flag set; render doesn't need it).
- Sim libs additionally: `-fno-builtin` (so a `sqrt` can't be silently inlined from a libm
  builtin), `-ffreestanding`-style include firewall via `-nostdinc++`.

---

## 7b. The macro catalogue (every macro in the codebase, its owner, its tier behaviour)

| Macro | Header | debug/dev | netcode/ship |
|---|---|---|---|
| `TL_ASSERT(c)` | `foundation/tl_assert.h` | check → fatal | compiled out |
| `TL_CHECK(c)` | same | check → fatal | check → fatal (slim tier) |
| `TL_FATAL(fmt, …)` | same | crash pipeline + abort | same |
| `TL_FIELDS_X(X,XA,XH)`, `TL_COMPONENT`, `TL_POOL_ROW`, `TL_WIRE_STRUCT` | `core/reflect.h` | as `ECS.md` §10.2 | same |
| `TL_LOG_{TRACE,DEBUG,INFO,WARN,ERR}` | `foundation/tl_log.h` | all levels | `INFO+` (ship: `WARN+`) |
| `TL_PROF_SCOPE(name)`, `TL_PROF_COUNTER(name, v)` | `foundation/tl_prof.h` | active | compiled out |
| `TL_PROBE_*` | `foundation/tl_probe.h` | active | compiled out |
| `TL_CVAR(type, name, default, flags, help)` | `core/cvar.h` | registered + console | `SIM` cvars registered (fingerprinted); others constant-folded |
| `TL_TEST(name, tags)`, `TL_EXPECT_*`, `TL_ASSERT_*` | `tests/runner/tl_test.h` | tests only | — |
| `TL_SCRATCH_SCOPE(s)` | `foundation/scratch.h` | mark/reset pair (explicit begin/end macros, no RAII) | same |
| `"lit"_id` | `foundation/hash.h` | constexpr FNV-1a | same |

No other macros are introduced without a row here.

## 8. Templates of code that appear everywhere (reference shapes)

```cpp
// a registered component — the X-macro declares struct + field table (ECS.md §6)
#define TL_FIELDS_Transform(X, XA, XH) \
    X(pos_t, x) X(pos_t, y) X(angle_t, rot) X(u32, flags)   /* bit 0 = snap (FRAME-LOOP.md §4); bits 1..31 zero */
TL_COMPONENT(Transform)

// a system — stateless free function + descriptor
void sys_move(World* w);
static const SystemDesc SYS_MOVE = { sys_move, "move"_id, PHASE_UPDATE,
                                     reads(COMP_Velocity), writes(COMP_Transform), {}, {} };

// a recoverable call
Result<TexHandle> r = asset_load_texture(w, "player.png"_id);
if (r.err) { TL_LOG_ERR(ERR_NAME(r.err)); return r.err; }
```

---

## 9. Rulings (closed 2026-08-22 — nothing open)

- **R-1 `[[nodiscard]]` is mandatory** on `Result<T>` and `ErrCode`. clang and clang-cl both honour
  it under `-Werror`; an ignored result is a compile error. The sanctioned "I know this can't
  fail" form is `TL_CHECK(call().err == 0)`.
- **R-3 The panic ABI is the one sanctioned callgraph out of sim code.** `TL_ASSERT`/`TL_CHECK`/
  `TL_FATAL` in `src/sim/` and the det half of `src/foundation/` resolve to a closed set of
  symbols - `tl_fatal`, `tl_check_failed`, `tl_assert_failed` - that the symbol audit allowlists
  by name even though they are defined in `tl_foundation`, above the audited layers, and reach
  io. This is deliberate and bounded: the panic path terminates the process, so it never executes
  inside a deterministic tick, and if it ever does, determinism has already ended. Without it the
  first `TL_CHECK` in `fx.h` would fail the audit either as a banned io callgraph or as an upward
  layer reference, which the second W0 review found before the fx lane started. Alternative
  rejected: a function pointer installed at boot - that is a byte of `.data`, which is exactly
  what the same audit forbids.
- **R-2 One field list, three doors.** Every reflected struct declares one `TL_FIELDS_Name(X,XA,XH)`
  list. Three macros consume it: `TL_COMPONENT(Name)` (ECS column + registration), `TL_POOL_ROW(Name)`
  (Alloy/engine pool rows: trivially-copyable + explicit-padding static_asserts + field table for the
  desync dump, no column), and `TL_WIRE_STRUCT(Name)` (adds the leading `u32 format_version`, an
  `offsetof` static_assert per field generated from the list, and the little-endian write/read
  pair). Same table, same kinds, same inspector; nothing is declared twice.
- **R-4 (RR-7) The tooling plane is exempt from the writable-static ban and reaches io directly.**
  `TOOLING.md` §9's runtimes (`log`, `prof`, `probe`, `crash`, plus `tl_assert` for the
  panic path's own writes) are named, individually, on `TL_FOUNDATION_TOOLING`
  (`src/foundation/CMakeLists.txt`) — a strict subset of the non-det stem list `BUILD.md` §10.2
  already splits out. Both `tools/audit/includes.py` (the `<stdio.h>`/`<stdlib.h>` allowance) and
  `tools/audit/symbols.py` (the `.data`/`.bss` exemption) parse that one line, so the exemption
  can only widen by editing it, never by a second hand-typed copy drifting out of sync
  (`LESSONS.md` has that drift class twice already). A sibling non-det stem not on the list (e.g.
  `jobs`, `mem_pool`) inherits nothing — the negative fixtures in `tools/audit/selftest.py` prove
  the same source under a non-tooling stem name still fails both gates, and a sim TU still cannot
  include anything but `tl_assert.h` (R-3's exemption is unchanged). §1's row states *why* this is
  sound and not a hole: the tooling plane is never hashed, never snapshotted, and never part of a
  world's registered arena set, so it carries none of the two-worlds-one-process reasoning the ban
  exists for. This also settles the seam R-3 explicitly left unresolved for the *det* side ("a
  function pointer installed at boot… is exactly what the same audit forbids"): in the exempted
  tooling plane, that objection does not apply, so a boot-installed callback (the crash-writer
  install slot `TOOLING.md` §9.3.9 describes) is the correct, sanctioned way for `tl_fatal` to
  reach `platform.crash.raise_fatal` once `platform/` exists — not a workaround, the seam.

*Rev 1 — 2026-08-22, reconciled 2026-08-24 (§1 exempts the tooling plane, §9 adds R-4); §3 names
the header owning `Result<T>`, §1 corrects the static-mutable gate and adds `memmove`, §9 R-3
rules the panic ABI (W0 skeleton and its two adversarial reviews, 2026-08-22).*
