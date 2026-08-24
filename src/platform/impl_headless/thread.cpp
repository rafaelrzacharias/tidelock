// thread.cpp - the headless ThreadApi (docs/PLATFORM.md §9.4: real, OS-direct - CreateThread/
// pthread_create, CreateSemaphoreW/sem_init, SRWLOCK/pthread_mutex, one TU with #ifdef branches).
// Handle slot tables are hand-rolled (docs/PLATFORM.md §9.3's ThreadRec[64]/SemRec[256]/
// MutexRec[256] caps), not SlotMap<T> - the containers lane has not landed (headless_state.h).
#include "platform/impl_headless/headless_apis.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#endif

namespace {

struct ThreadTrampolineArgs { ThreadFn fn; void* ctx; };

#ifdef _WIN32
DWORD WINAPI win_trampoline(LPVOID param) {
    ThreadTrampolineArgs* a = (ThreadTrampolineArgs*)param;
    a->fn(a->ctx);
    return 0;
}
#else
void* posix_trampoline(void* param) {
    ThreadTrampolineArgs* a = (ThreadTrampolineArgs*)param;
    a->fn(a->ctx);
    return nullptr;
}
#endif

Result<ThreadHandle> ht_create(void* ctx, ThreadFn fn, void* tctx, StrView, u32 stack_bytes) {
    HeadlessState* s = (HeadlessState*)ctx;
    u32 idx = HEADLESS_MAX_THREADS;
    for (u32 i = 0; i < HEADLESS_MAX_THREADS; ++i) { if (!s->threads[i].alive) { idx = i; break; } }
    if (idx == HEADLESS_MAX_THREADS) {
        return Result<ThreadHandle>{ ThreadHandle{}, (ErrCode)ERR_PLATFORM_THREAD_LIMIT };
    }
    ThreadTrampolineArgs* args = (ThreadTrampolineArgs*)arena_push(&s->arena, sizeof(ThreadTrampolineArgs), alignof(ThreadTrampolineArgs));
    args->fn = fn; args->ctx = tctx;
    const u32 stack = stack_bytes != 0u ? stack_bytes : (1u << 20);
#ifdef _WIN32
    HANDLE h = CreateThread(nullptr, stack, win_trampoline, args, 0, nullptr);
    if (h == nullptr) { return Result<ThreadHandle>{ ThreadHandle{}, (ErrCode)ERR_PLATFORM_THREAD_CREATE }; }
    s->threads[idx].os_handle = (void*)h;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t tid;
    const int rc = pthread_create(&tid, &attr, posix_trampoline, args);
    pthread_attr_destroy(&attr);
    if (rc != 0) { return Result<ThreadHandle>{ ThreadHandle{}, (ErrCode)ERR_PLATFORM_THREAD_CREATE }; }
    s->threads[idx].os_thread = (unsigned long long)tid;
#endif
    if (s->thread_gen[idx] == 0u) { s->thread_gen[idx] = 1u; }
    s->threads[idx].alive = 1u; s->threads[idx].fn = fn; s->threads[idx].fn_ctx = tctx;
    return Result<ThreadHandle>{ handle_make<ThreadHandle>(idx, (u32)s->thread_gen[idx]), ERR_OK };
}

void ht_join(void* ctx, ThreadHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (handle_is_null(h)) { return; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_THREADS || !s->threads[idx].alive || handle_gen(h) != s->thread_gen[idx]) { return; }
#ifdef _WIN32
    WaitForSingleObject((HANDLE)s->threads[idx].os_handle, INFINITE);
    CloseHandle((HANDLE)s->threads[idx].os_handle);
#else
    pthread_join((pthread_t)s->threads[idx].os_thread, nullptr);
#endif
    s->threads[idx].alive = 0u;
    s->thread_gen[idx] = headless_gen_next<ThreadHandle>(s->thread_gen[idx]);   // wrap-to-1 (headless_state.h)
}

// ErrPlatform has no dedicated SEM/MUTEX "_LIMIT" code (docs/PLATFORM.md §9.2's enum); resource
// exhaustion in either table is reported as ERR_PLATFORM_THREAD_LIMIT, the closest existing
// bucket - "a thread-primitives table is full", same class of failure as the thread table itself.
Result<SemHandle> ht_sem_create(void* ctx, u32 initial) {
    HeadlessState* s = (HeadlessState*)ctx;
    u32 idx = HEADLESS_MAX_SEMS;
    for (u32 i = 0; i < HEADLESS_MAX_SEMS; ++i) { if (!s->sems[i].alive) { idx = i; break; } }
    if (idx == HEADLESS_MAX_SEMS) { return Result<SemHandle>{ SemHandle{}, (ErrCode)ERR_PLATFORM_THREAD_LIMIT }; }
#ifdef _WIN32
    HANDLE h = CreateSemaphoreW(nullptr, (LONG)initial, 0x7fffffffL, nullptr);
    if (h == nullptr) { return Result<SemHandle>{ SemHandle{}, (ErrCode)ERR_PLATFORM_THREAD_CREATE }; }
    s->sems[idx].os_sem = (void*)h;
#else
    sem_t* sem = (sem_t*)arena_push(&s->arena, sizeof(sem_t), alignof(sem_t));
    if (sem_init(sem, 0, initial) != 0) { return Result<SemHandle>{ SemHandle{}, (ErrCode)ERR_PLATFORM_THREAD_CREATE }; }
    s->sems[idx].os_sem = (void*)sem;
#endif
    if (s->sem_gen[idx] == 0u) { s->sem_gen[idx] = 1u; }
    s->sems[idx].alive = 1u;
    return Result<SemHandle>{ handle_make<SemHandle>(idx, (u32)s->sem_gen[idx]), ERR_OK };
}

void* sem_os(HeadlessState* s, SemHandle h) {
    if (handle_is_null(h)) { return nullptr; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_SEMS || !s->sems[idx].alive || handle_gen(h) != s->sem_gen[idx]) { return nullptr; }
    return s->sems[idx].os_sem;
}

void ht_sem_wait(void* ctx, SemHandle h) {
    void* os = sem_os((HeadlessState*)ctx, h);
    if (os == nullptr) { return; }
#ifdef _WIN32
    WaitForSingleObject((HANDLE)os, INFINITE);
#else
    while (sem_wait((sem_t*)os) != 0 && errno == EINTR) {}
#endif
}

u8 ht_sem_try_wait(void* ctx, SemHandle h) {
    void* os = sem_os((HeadlessState*)ctx, h);
    if (os == nullptr) { return 0u; }
#ifdef _WIN32
    return WaitForSingleObject((HANDLE)os, 0) == WAIT_OBJECT_0 ? 1u : 0u;
#else
    return sem_trywait((sem_t*)os) == 0 ? 1u : 0u;
#endif
}

void ht_sem_post(void* ctx, SemHandle h) {
    void* os = sem_os((HeadlessState*)ctx, h);
    if (os == nullptr) { return; }
#ifdef _WIN32
    ReleaseSemaphore((HANDLE)os, 1, nullptr);
#else
    sem_post((sem_t*)os);
#endif
}

void ht_sem_destroy(void* ctx, SemHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (handle_is_null(h)) { return; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_SEMS || !s->sems[idx].alive || handle_gen(h) != s->sem_gen[idx]) { return; }
#ifdef _WIN32
    CloseHandle((HANDLE)s->sems[idx].os_sem);
#else
    sem_destroy((sem_t*)s->sems[idx].os_sem);
#endif
    s->sems[idx].alive = 0u;
    s->sem_gen[idx] = headless_gen_next<SemHandle>(s->sem_gen[idx]);   // wrap-to-1 (headless_state.h)
}

Result<MutexHandle> ht_mutex_create(void* ctx) {
    HeadlessState* s = (HeadlessState*)ctx;
    u32 idx = HEADLESS_MAX_MUTEXES;
    for (u32 i = 0; i < HEADLESS_MAX_MUTEXES; ++i) { if (!s->mutexes[i].alive) { idx = i; break; } }
    if (idx == HEADLESS_MAX_MUTEXES) { return Result<MutexHandle>{ MutexHandle{}, (ErrCode)ERR_PLATFORM_THREAD_LIMIT }; }
#ifdef _WIN32
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)arena_push(&s->arena, sizeof(CRITICAL_SECTION), alignof(CRITICAL_SECTION));
    InitializeCriticalSection(cs);
    s->mutexes[idx].os_mutex = (void*)cs;
#else
    pthread_mutex_t* m = (pthread_mutex_t*)arena_push(&s->arena, sizeof(pthread_mutex_t), alignof(pthread_mutex_t));
    pthread_mutex_init(m, nullptr);
    s->mutexes[idx].os_mutex = (void*)m;
#endif
    if (s->mutex_gen[idx] == 0u) { s->mutex_gen[idx] = 1u; }
    s->mutexes[idx].alive = 1u;
    return Result<MutexHandle>{ handle_make<MutexHandle>(idx, (u32)s->mutex_gen[idx]), ERR_OK };
}

void* mutex_os(HeadlessState* s, MutexHandle h) {
    if (handle_is_null(h)) { return nullptr; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_MUTEXES || !s->mutexes[idx].alive || handle_gen(h) != s->mutex_gen[idx]) { return nullptr; }
    return s->mutexes[idx].os_mutex;
}

void ht_mutex_lock(void* ctx, MutexHandle h) {
    void* os = mutex_os((HeadlessState*)ctx, h);
    if (os == nullptr) { return; }
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION*)os);
#else
    pthread_mutex_lock((pthread_mutex_t*)os);
#endif
}

void ht_mutex_unlock(void* ctx, MutexHandle h) {
    void* os = mutex_os((HeadlessState*)ctx, h);
    if (os == nullptr) { return; }
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION*)os);
#else
    pthread_mutex_unlock((pthread_mutex_t*)os);
#endif
}

void ht_mutex_destroy(void* ctx, MutexHandle h) {
    HeadlessState* s = (HeadlessState*)ctx;
    if (handle_is_null(h)) { return; }
    const u32 idx = handle_index(h);
    if (idx >= HEADLESS_MAX_MUTEXES || !s->mutexes[idx].alive || handle_gen(h) != s->mutex_gen[idx]) { return; }
#ifdef _WIN32
    DeleteCriticalSection((CRITICAL_SECTION*)s->mutexes[idx].os_mutex);
#else
    pthread_mutex_destroy((pthread_mutex_t*)s->mutexes[idx].os_mutex);
#endif
    s->mutexes[idx].alive = 0u;
    s->mutex_gen[idx] = headless_gen_next<MutexHandle>(s->mutex_gen[idx]);   // wrap-to-1 (headless_state.h)
}

void ht_yield(void*) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

void ht_sleep_ms(void*, u32 ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, nullptr);
#endif
}

u32 ht_core_count(void*) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (u32)si.dwNumberOfProcessors;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1u;
#endif
}

u8 ht_is_main(void* ctx) {
    return headless_current_thread_id() == ((HeadlessState*)ctx)->main_os_tid ? 1u : 0u;
}

}  // namespace

u64 headless_current_thread_id() {
#ifdef _WIN32
    return (u64)GetCurrentThreadId();
#else
    return (u64)pthread_self();
#endif
}

ThreadApi headless_thread_api(HeadlessState* s) {
    return ThreadApi{ s, ht_create, ht_join,
                       ht_sem_create, ht_sem_wait, ht_sem_try_wait, ht_sem_post, ht_sem_destroy,
                       ht_mutex_create, ht_mutex_lock, ht_mutex_unlock, ht_mutex_destroy,
                       ht_yield, ht_sleep_ms, ht_core_count, ht_is_main };
}
