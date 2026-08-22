# Jobs — worker-invariant parallelism (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22. Shape **DECIDED** (PIVOT §12a); built post-v0, before any
> parallel Alloy code. v0 ships the API single-threaded. INV-7 (worker-count invariance:
> `NETCODE.md` §2) is the requirement this module exists to satisfy.
> **Owns:** `src/foundation/jobs.h`, `atomic.h`.

---

## 0. The one rule everything follows from (DECIDED)

> **Outputs are keyed by CHUNK id, never by worker id.**

`parallel_for(range, grain)` splits `[0, N)` into chunks by a pure function of `(N, grain)` — never
of worker count, timing, or arrival. Per-chunk outputs land in chunk-indexed slots; merges and
reductions fold in chunk-index order. Worker identity is invisible to results, which means
**work-stealing is permitted for free** — scheduling affects wall time only. (Stronger than Ore
`jobs`' worker-index merge, which forbade stealing.)

Corollaries: per-worker scratch arenas and per-worker command buffers exist for allocation
locality, but command buffers are *tagged by chunk id* and applied at the barrier in chunk order;
event rings likewise. Integer/fx sums are order-free anyway; the chunk-order fold is kept as the
rule so nothing breaks if a widened or non-commutative combine ever appears.

---

## 1. API (DECIDED)

```cpp
typedef void (*ChunkFn)(void* ctx, u32 chunk, u32 begin, u32 end, Scratch* scratch);
void parallel_for(Jobs*, u32 n, u32 grain, ChunkFn fn, void* ctx);            // barrier on return
// reductions: caller allocates partials[chunk_count(n, grain)] on scratch, fn writes partials[chunk], caller folds 0..count
u32  chunk_count(u32 n, u32 grain);                                             // pure function of (n, grain)
void parallel_levels(Jobs*, u32 level_count, const Level* levels, ChunkFn fn, void* ctx);  // colored GS host: levels sequential, parallel_for within
```

- **Colored Gauss-Seidel host:** colors become sequential levels; within a level, `parallel_for`
  over the color's constraint list in stable-id order chunks (`ALLOY.md` §8.1's coloring rules
  unchanged: persistent constraints color once, contacts recolor per tick, deterministic greedy
  in stable-id order).
- **System-level parallelism:** within a phase, `SystemDesc.reads/writes` build parallel groups at
  schedule-build time; non-conflicting systems run as one job each (chunk = system), conflicting
  ones serialize in the fixed order. Conflict detection is fatal at startup only for *declared*
  overlap; undeclared access is caught by the debug access checker (`ECS.md` §3).
- The main thread participates as worker 0; `parallel_for` on one worker is a plain loop (v0).
- `Scratch*` is the worker's scratch, passed explicitly; no `thread_local`.

---

## 2. Implementation (DECIDED — alternatives recorded)

| Option | Determinism | Performance | LOC / cognitive | Correctness surface |
|---|---|---|---|---|
| **A. fixed pool + atomic chunk counter** (chosen for v1) | trivially invariant (chunking is a pure fn; the counter only decides *who* runs a chunk) | near-ideal for flat `parallel_for` over uniform chunks; contention on one counter is negligible at ≤16 workers × thousands of chunks | ~200 lines | one atomic, one semaphore, one barrier |
| B. Chase-Lev work-stealing deques | invariant under the chunk rule | better for irregular/nested jobs (island-sized chunks vary 100×) | ~600 lines + the ABA/memory-order subtlety | the classic lock-free bug surface |
| C. task graph with dependencies (fibers) | invariant if merges are keyed | covers dynamic dependency chains | large | rejected: the phase/level model *is* the dependency structure; fibers need platform asm |

A is enough for Alloy's shape (levels of uniform work) and for per-chunk sim-view writes. B is
the named upgrade if island-scoped work (T-A-01 rollback, pass 5) shows tail latency from uneven
chunks; the API does not change. C is rejected, not deferred.

Pool: `core_count − 1` persistent workers, spin briefly then semaphore-wait; a job epoch counter
wakes them; the barrier is a countdown on completed chunks. Per-worker scratch from `MEMORY.md`
§1.3.

---

## 3. Gates (DECIDED — T-F-02 transfers verbatim)

- Identical hash trace at **1 / 2 / 8 / 16 workers** is a **blocking release gate**.
- One **mixed-pair** run once transport exists: peer A at 4 workers, peer B at 16, same inputs.
- A debug "shuffle" mode randomizes chunk→worker assignment and chunk completion order every run
  (legally nondeterministic — its own PRNG); any hash change under shuffle is a keyed-by-worker
  bug made visible.
- The symbol gate applies to worker code: no clock, no alloc inside a chunk fn.

---

## 4. Where the parallelism goes first

Alloy pass 3 (colored GS over constraints), pass 1 (fields over chunks), broadphase rebuild (cells
→ radix sort), the per-arena hash (arenas as chunks), and the sim-view writer (chunks). Gameplay
systems parallelize by `reads/writes` groups for free once the pool exists.

---

## 5. Open

- **O-1** Grain selection: a constant per call site vs `n / (workers × 4)`. The latter makes chunk
  *boundaries* depend on worker count — **forbidden**. Grain is a per-call-site constant (or a
  pure function of `n`), stated in `chunk_count`.
- **O-2** Whether the Pi's 4 cores get a smaller default pool (3 workers) or the same code path
  with `core_count − 1`: the same path; no per-platform branch.

*Rev 1 — 2026-08-22.*
