#pragma once
// ---------------------------------------------------------------------------------------------
// thread_api.h - ThreadApi: the platform's thread/semaphore/mutex table, in foundation.
//
// Spec: docs/PLATFORM.md §9.2 (this is that struct, transcribed verbatim); docs/PLATFORM.md §6
//   (primitives only - the job system is docs/JOBS.md); docs/ARCHITECTURE.md §1 rule 1.
// Purpose: give ThreadApi ONE definition that foundation can see. docs/JOBS.md §6.2's `Jobs`
//   holds ThreadHandles and calls through this table, and foundation is a leaf - it can never
//   include platform/platform.h (tools/audit/includes.py MODULE_DAG). This is the VMemApi case
//   verbatim (foundation/vmem_api.h, docs/MEMORY.md §8.2): the struct's one home is the
//   foundation-visible header, and platform.h INCLUDES it rather than redefining it.
// Invariants: every verb takes the table's own `void* ctx` first; the impl holds no static
//   mutable state (docs/CPP-SUBSET.md §1). Handle generation is 4 bits and wraps to 1, never 0
//   (docs/PLATFORM.md §9.4).
// Determinism: NOTHING here may be read back into sim state. Thread scheduling is explicitly on
//   the "free to be non-deterministic" side of docs/DETERMINISM.md §0; worker identity is
//   invisible to results by the chunk-keyed rule (docs/JOBS.md §0).
// Threading: the CREATE/DESTROY verbs (`create`, `join`, `sem_create`, `sem_destroy`,
//   `mutex_create`, `mutex_destroy`) are NOT thread-safe - every impl keeps its slot tables in
//   unsynchronised state and allocates from a single-writer arena. Call them from the owning
//   thread before the pool starts, which is what docs/JOBS.md does. The USE verbs (`sem_wait`/
//   `sem_post`/`sem_try_wait`, `mutex_lock`/`mutex_unlock`, `yield`, `sleep_ms`, `core_count`,
//   `is_main`) are safe from any thread; that is their point.
// Includes: foundation/{tl_types,handle,strview}.h only.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/handle.h"
#include "foundation/strview.h"

typedef Handle<struct ThreadTag, 12, 4> ThreadHandle;
typedef Handle<struct SemTag, 12, 4>    SemHandle;
typedef Handle<struct MutexTag, 12, 4>  MutexHandle;

typedef void (*ThreadFn)(void* ctx);

struct ThreadApi {
    void* ctx;
    Result<ThreadHandle> (*create)(void* ctx, ThreadFn, void* tctx, StrView name, u32 stack_bytes /*0 -> 1 MB*/);
    void (*join)(void* ctx, ThreadHandle);
    Result<SemHandle> (*sem_create)(void* ctx, u32 initial); void (*sem_wait)(void* ctx, SemHandle); u8 (*sem_try_wait)(void* ctx, SemHandle);
    void (*sem_post)(void* ctx, SemHandle); void (*sem_destroy)(void* ctx, SemHandle);
    Result<MutexHandle> (*mutex_create)(void* ctx); void (*mutex_lock)(void* ctx, MutexHandle); void (*mutex_unlock)(void* ctx, MutexHandle); void (*mutex_destroy)(void* ctx, MutexHandle);
    void (*yield)(void* ctx); void (*sleep_ms)(void* ctx, u32); u32 (*core_count)(void* ctx); u8 (*is_main)(void* ctx);
};
