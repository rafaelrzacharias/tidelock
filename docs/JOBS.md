# Jobs — worker-invariant parallelism (tidelock, rev 1)

> **Status:** design rev 1, 2026-08-22; reconciled against the built headers 2026-08-24 (W1 jobs
> lane). Shape **DECIDED** (PIVOT §12a); built post-v0, before any
> parallel Alloy code. v0 ships the API single-threaded. INV-7 (worker-count invariance:
> `NETCODE.md` §2) is the requirement this module exists to satisfy.
> **Owns:** `src/foundation/jobs.h`, `atomic.h`, `thread_api.h` (the last only because foundation
> is a leaf and cannot include `platform/platform.h` — the struct is `PLATFORM.md` §9.2's, the
> home is here, exactly as `foundation/vmem_api.h` holds `VMemApi`).

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
// reductions: caller allocates partials[jobs_chunk_count(n, grain)] on scratch, fn writes partials[chunk], caller folds 0..count
u32  jobs_chunk_count(u32 n, u32 grain);                                        // pure function of (n, grain)
void parallel_levels(Jobs*, u32 level_count, const Level* levels, LevelFn fn, void* ctx);  // colored GS host: levels sequential, parallel_for within (LevelFn: R-5)
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

Pool: `core_count − 1` persistent workers (clamped, R-6) waiting on a semaphore each (R-3); a job
epoch counter publishes the job; the barrier is a countdown on **participants** (R-4). Per-worker
scratch from `MEMORY.md` §1.3.

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

## 5. Rulings (closed; R-3..R-6 added 2026-08-24 — nothing open)

- **R-1 Grain is a per-call-site constant** (or a pure function of `n` alone), stated at the call
  and folded by `chunk_count(n, grain)`. Any grain derived from worker count makes chunk
  *boundaries* worker-dependent and is forbidden — the debug shuffle mode (§3) would catch it.
- **R-2 One pool policy on every platform:** `core_count − 1` workers, main thread participates.
  No per-platform branch; the Pi simply runs 3 workers. Worker count never enters results.

Added 2026-08-24 (W1 jobs lane), each a defect in rev 1's own §6.2/§6.3 found before code:

- **R-3 One semaphore per worker, never one shared `wake`.** Rev 1 posted a shared semaphore
  `min(worker_count, c)` times. Semaphore tokens are fungible: a worker that finishes a job,
  loops, and immediately consumes a *second* token for the same epoch (it sees `e == seen` and
  parks again) has eaten a token meant for a worker that then never wakes — and with a
  chunk-countdown barrier that is a **permanent hang**, not a slow tick. Per-worker semaphores
  make participation exact and delete the whole token-stealing class. Cost: 31 semaphores against
  `PLATFORM.md` §9.3's `SemRec[256]`.
- **R-4 The barrier counts PARTICIPANTS, not chunks.** Rev 1 counted down `done_chunks` and let
  `parallel_for` return when it hit 0 — but the worker that took it to 0 is *still inside its
  claim loop* and will execute one more `atomic_fetch_add(next_chunk)`. By then the main thread
  may have published the next job and reset `next_chunk` to 0, so that stale worker claims chunk
  0 of job N+1 **and runs it with job N's `fn`/`ctx`**. Counting participants (each decrements
  once, after its loop has exited) makes "pending == 0" mean "no thread can touch `next_chunk`",
  which is the property the publish/reset actually needs. Each participant also snapshots
  `JobDesc` into a local under the epoch acquire, so no loop re-reads a field the main thread may
  be rewriting. The post/wait is balanced exactly once: only a worker posts `done`, and only when
  its decrement returns 1; main waits iff its own decrement did not return 1 — so no stale token
  can survive a job.
- **R-5 `parallel_levels` needs its own fn type.** Rev 1 wrote `parallel_for(..., fn, (ctx, l))`
  through a `ChunkFn` that has no level parameter — not expressible. `LevelFn` takes `u32 level`
  explicitly and `struct Level { u32 count; u32 grain; }` is spelled out; the level index reaches
  the callee through a `LevelCtx` on `parallel_levels`' own stack frame, which outlives every
  level because each `parallel_for` is a barrier.
- **R-6 `core_count − 1` is clamped to `JOBS_MAX_WORKERS`.** Rev 1 stated the policy and a
  31-entry `threads[]` and never reconciled them: a 32-core machine overruns the array on its
  first `jobs_init`. `jobs_default_worker_count` is the one home for the clamp, and it is a
  separate call rather than an "auto" sentinel so the count a pool runs at is always a value the
  caller can see and size its scratch array against.

## 6. Implementation specification

### 6.1 Files

`foundation/jobs.h/.cpp` (pool, `parallel_for`, `parallel_levels`, `chunk_count`),
`foundation/atomic.h`, worker threads via `PLATFORM.md` §6 through `foundation/thread_api.h`.

**`foundation/atomic.h` — the closed API** (this is its one home; rev 1 spelled it
`tl_atomic_load/store/fetch_add/cas` here and a different, fuller set in `PLATFORM.md` §9.2, which
is drift: two names for one header. The §9.2 spelling wins — it states widths and orders — and
§9.2 should cite this section rather than restate it):

```cpp
u32/u64  atomic_load32/64(const T* p);          // ACQUIRE
void     atomic_store32/64(T* p, T v);          // RELEASE
u32/u64  atomic_add32/64(T* p, T v);            // ACQ_REL, returns the PREVIOUS value
u32/u64  atomic_sub32/64(T* p, T v);            // ACQ_REL, returns the PREVIOUS value
u8       atomic_cas32/64(T* p, T expect, T desire);   // strong; ACQ_REL success, ACQUIRE failure
u32/u64  atomic_exchange32/64(T* p, T v);       // ACQ_REL
void     atomic_fence_acquire/release/seq_cst();
```

Each is a `static inline` over `__atomic_*_n` / `__atomic_compare_exchange_n` /
`__atomic_thread_fence` with the order **fixed in the function**, so a call site cannot weaken
one by passing the wrong constant; §6.3's chunk counter is therefore ACQ_REL rather than the
RELAXED the rev-1 pseudocode spelled — stronger, correct, and free on x86-64. `p` must be
naturally aligned (a misaligned atomic is UB, and a fault on aarch64); `Jobs` `static_assert`s its
own offsets. No `<atomic>`, no `volatile`. Both `atomic.h` and `jobs.h` `#error` under `TL_SIM_TU`:
`tools/audit/allow.txt` names `__aarch64_*` outline atomics as the tripwire for atomics in det
code, but on x86-64 a 32-bit fetch-add inlines to `lock xadd` and emits no symbol at all, so that
tripwire exists only on the Pi leg — where the symbol audit does not run. A compile error runs
everywhere.

### 6.2 Structures

```cpp
typedef void (*ChunkFn)(void* ctx, u32 chunk, u32 begin, u32 end, Scratch*);
typedef void (*LevelFn)(void* ctx, u32 level, u32 chunk, u32 begin, u32 end, Scratch*);  // R-5
struct Level { u32 count; u32 grain; };                                                  // R-5
struct JobsConfig { const ThreadApi* thread; Scratch* const* scratch;
                    u32 scratch_count; u32 worker_count; u32 stack_bytes; u32 _pad0; };
struct JobDesc { ChunkFn fn; void* ctx; u64 shuffle_key; u32 n; u32 grain; u32 chunks; u32 _pad0; };
struct JobsWorker { Jobs* jobs; u32 index; u32 _pad0; };     // a worker's ThreadFn argument
struct Jobs {
    const ThreadApi* thread;
    Scratch*     scratch[JOBS_MAX_WORKERS + 1];   // index 0 = the calling thread's
    JobsWorker   slots[JOBS_MAX_WORKERS];
    ThreadHandle threads[JOBS_MAX_WORKERS];
    SemHandle    wake[JOBS_MAX_WORKERS];          // ONE PER WORKER — R-3
    SemHandle    done;
    u64          shuffle_seed;   u32 worker_count;  u32 _pad0;
    // per job (one at a time — jobs never nest; a chunk fn calling parallel_for is TL_FATAL)
    JobDesc  job;
    u32 epoch;       u32 _line0[15];   // atomic; published RELEASE to publish a job
    u32 next_chunk;  u32 _line1[15];   // atomic claim ticket
    u32 pending;     u32 _line2[15];   // atomic countdown of PARTICIPANTS, not chunks — R-4
    u32 shutdown;    u32 in_job;       // atomic; in_job is the nested-parallel_for tripwire
};
u32  jobs_chunk_count(u32 n, u32 grain);   // pure function of (n, grain); 64-bit intermediate
u32  jobs_default_worker_count(const ThreadApi*);   // R-2's policy, clamped — R-6
```

Names: `parallel_for`/`parallel_levels` keep the spelling §1 and four consumer docs cite;
everything else carries the module prefix (`jobs_chunk_count`, `JOBS_MAX_WORKERS`), matching
`arena_push`/`scratch_push`/`SCRATCH_MAX_SCOPES` — `MAX_WORKERS` and `chunk_count` are too
ordinary for a header every module can reach. The three hot atomics sit 64 B apart so one
worker's claim cannot invalidate another counter's cache line; `Jobs` is **pinned** once
initialised (`slots` hold a back-pointer into it), and is caller-allocated because foundation
holds no static mutable state (`CPP-SUBSET.md` §1).

### 6.3 Algorithms

```
parallel_for(j, n, grain, fn, ctx):
    if in_job != 0: TL_FATAL                                    // jobs never nest
    c = jobs_chunk_count(n, grain); if c == 0: return
    in_job = 1
    epoch += 1 (RELEASE); key = shuffle_seed ? mix(shuffle_seed, epoch) : 0   // every job, inline or not
    if worker_count == 0 || c == 1:
        for t in 0..c: run_one({fn,ctx,key,n,grain,c}, t, scratch[0])   // shuffle reorders this path too
    else:
        job = {fn, ctx, key, n, grain, c}; next_chunk = 0 (RELEASE)
        pending = worker_count + 1 (RELEASE)                    // PARTICIPANTS, not chunks — R-4
        atomic_store(epoch, epoch + 1, RELEASE)                 // publishes `job` to every worker
        for w in 1..worker_count: wake[w-1].post()              // one post per worker — R-3
        claim_loop(j, /*worker*/ 0)                             // the calling thread participates
        if atomic_sub(pending, 1) != 1: done.wait()             // exactly one post, exactly one wait
    in_job = 0
claim_loop(j, w):
    local = job                                                 // snapshot under the epoch acquire
    loop: t = atomic_add(next_chunk, 1); if t >= local.chunks: break
          run_one(local, t, scratch[w]); if local.shuffle_key: jitter(t)
    if w != 0 and atomic_sub(pending, 1) == 1: done.post()      // only a worker posts; main waits
run_one(job, t, scratch):                                       // the ONE ticket -> chunk mapping
    k = job.shuffle_key ? permute(t, job.chunks, job.shuffle_key) : t
    job.fn(job.ctx, k, k*job.grain, min(job.n,(k+1)*job.grain), scratch)
worker_main(slot):
    seen = 0
    loop: wake[slot.index-1].wait(); if atomic_load(shutdown): return
          e = atomic_load(epoch, ACQUIRE); if e == seen: continue; seen = e
          claim_loop(slot.jobs, slot.index)
parallel_levels(j, L, levels, fn, ctx):                         // R-5
    for l in 0..L: parallel_for over levels[l], trampolining (fn, ctx, l) through a stack LevelCtx
```

Scratch reset for workers happens at the barrier (`FRAME-LOOP.md` §3), never inside a job:
`jobs_scratch_reset_all` is that verb (step 4 of the barrier order).
"Shuffle" mode: `run_one` maps its ticket through a seeded index bijection (`permute` = a balanced
4-round Feistel over an even `ceil(log2(chunks))` bits with cycle-walking, so it needs **no
permutation array** and works at any chunk count — a Feistel network is a bijection for any round
function, and bijectivity is the whole requirement: a fallback to the identity on a long walk
would run one chunk twice and another never), and `claim_loop` jitters completion order with a
`yield`, never a sleep. Results must not change. Its seed is the caller's (`jobs_shuffle_set`), so
a failing shuffle run replays exactly. Two deliberate departures from rev 1: it is compiled into
**every tier** (inert at seed 0; an `#if` would leave the mode untested in the tier that ships and
make its test tier-conditional), and it reorders the **inline path** too — a chunk fn that depends
on chunk order is a bug at `worker_count == 0` as much as at 16, and an inline path that always
ran `0,1,2,…` could never expose it. That is also what makes the §6.4 shuffle test non-racy: at
`worker_count == 0` the claim order is a pure function of the seed, so the test asserts the exact
schedule CHANGED rather than sampling a timing-dependent trace.

### 6.4 Tests (`tests/foundation/jobs.test.cpp`)

`jobs_chunk_count` table; `parallel_for` sum of chunk-keyed partials equals the serial sum for n
in {0, 1, grain−1, grain, 10⁶} at workers {0, 1, 2, 8, 16}; shuffle mode → identical results;
nested `parallel_for` → fatal-expected; levels run strictly in order (a chunk in level l+1 sees
every level-l write — tested with a dependency array); worker scratch isolation (each chunk
writes a marker in its worker scratch, no cross-talk); a 10⁵-job churn under TSan nightly.

Added 2026-08-24 with the rulings above, because each states a property no row above could fail on:

- **Bit-identity, not "same sum".** The sweep compares the whole chunk-keyed output BUFFER byte
  for byte across worker counts, not a reduced scalar: an integer sum is order-free, so a
  worker-keyed bug that reorders *which slot* a partial lands in still adds up (`LESSONS.md`: two
  instances running the same op sequence is the weakest determinism test that still looks like one).
- **Shuffle must be shown to have SHUFFLED.** "Results matched under shuffle" is vacuous if the
  scheduler happened to run the same order. The test records the claim sequence (each chunk fn
  appends its own index to a test-owned atomic-indexed trace) and asserts the trace **differs**
  from the unshuffled run while the output buffer is byte-identical. A shuffle run whose order did
  not change is a failed test, not a passed one.
- **Counter-pool exhaustion** (`worker_count > JOBS_MAX_WORKERS`, `scratch_count` short, the
  platform refusing a thread or a semaphore mid-init) returns its named code and leaves **no
  partial pool** — no thread outlives a failed `jobs_init`.
- **Scratch isolation and the `0xDD` poison**: each chunk writes a marker into the scratch it was
  handed and reads back only its own; `jobs_scratch_reset_all` poisons every participant's arena.
- **A contention soak** (many small jobs, all worker counts) is the measurement behind §6.2's
  cache-line separation, and is where a lost wake-up or a stale claim shows up as a hang. Every
  hanging-class test passes `--timeout-ms` to its child invocation rather than stalling the lane.

*Rev 1 — 2026-08-22; reconciled 2026-08-24 (W1 jobs lane): §6.1 carries the `atomic.h` API that
`PLATFORM.md` §9.2 had been restating, §6.2/§6.3 land R-3..R-6, §6.4 gains the shuffle-order and
contention rows.*
