/* Host coroutines for guest context switches (see guest_fiber.h). Upstream candidate. */
#include "guest_fiber.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sched.h>
#  include <time.h>
#  include <pthread.h>
#  include <ucontext.h>
#endif

struct guest_fiber {
    void* ctx;               /* the shared register block of the host thread */
    size_t ctx_size;
    void* saved;             /* this fiber's copy while it is not running */
    guest_fiber_body body;
    void* arg;
    guest_fiber* creator;    /* resumed when body returns */
    int is_thread;           /* the converted host thread itself: never freed */
    int locals[4];           /* this fiber's values of the registered per-fiber locals while it is not running */
#ifdef _WIN32
    void* h;
#else
    ucontext_t uc;
    void* stack;
#endif
};

#if defined(_MSC_VER)
static __declspec(thread) guest_fiber* t_cur;
static __declspec(thread) guest_fiber* t_thread;
#else
static __thread guest_fiber* t_cur;
static __thread guest_fiber* t_thread;
#endif

static int* (*g_locals[4])(void);
static int g_nlocals;

int guest_fiber_add_local(int* (*slot)(void))
{
    if (!slot || g_nlocals >= 4) return -1;
    g_locals[g_nlocals++] = slot;
    return 0;
}

static void gf_die(const char* what)
{
    fprintf(stderr, "[fiber] fatal: %s\n", what);
    fflush(stderr);
    abort();
}

static void gf_run(guest_fiber* f)
{
    /* guest_fiber_switch already loaded this fiber's block into ctx. */
    f->body(f->ctx, f->arg);
    /* The body returned: go back to the creator. Nobody may resume this fiber again. */
    guest_fiber_switch(f->creator);
    gf_die("finished fiber resumed");
}

#ifdef _WIN32
static void WINAPI gf_entry(void* p) { gf_run((guest_fiber*)p); }
#else
static void gf_entry(unsigned lo, unsigned hi)
{
    gf_run((guest_fiber*)(((uintptr_t)hi << 32) | (uintptr_t)lo));
}
#endif

guest_fiber* guest_fiber_self(void* ctx, size_t ctx_size)
{
    if (t_thread) {
        if (t_thread->ctx != ctx || t_thread->ctx_size != ctx_size) gf_die("thread used with two register blocks");
        return t_thread;
    }
    guest_fiber* f = (guest_fiber*)calloc(1, sizeof *f);
    if (!f) gf_die("out of memory");
    f->ctx = ctx; f->ctx_size = ctx_size; f->is_thread = 1;
    f->saved = malloc(ctx_size);
    if (!f->saved) gf_die("out of memory");
#ifdef _WIN32
    f->h = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    if (!f->h) gf_die("ConvertThreadToFiber failed");
#endif
    t_thread = t_cur = f;
    return f;
}

guest_fiber* guest_fiber_current(void) { return t_cur; }

guest_fiber* guest_fiber_create(guest_fiber_body body, void* arg, size_t stack_bytes)
{
    guest_fiber* cur = t_cur;
    if (!cur) gf_die("guest_fiber_create before guest_fiber_self");
    if (!stack_bytes) stack_bytes = (size_t)32 << 20;
    guest_fiber* f = (guest_fiber*)calloc(1, sizeof *f);
    if (!f) gf_die("out of memory");
    f->ctx = cur->ctx; f->ctx_size = cur->ctx_size; f->body = body; f->arg = arg; f->creator = cur;
    f->saved = malloc(f->ctx_size);
    if (!f->saved) gf_die("out of memory");
    memcpy(f->saved, f->ctx, f->ctx_size);
#ifdef _WIN32
    f->h = CreateFiberEx(256 << 10, stack_bytes, FIBER_FLAG_FLOAT_SWITCH, gf_entry, f);
    if (!f->h) gf_die("CreateFiberEx failed");
#else
    f->stack = malloc(stack_bytes);
    if (!f->stack || getcontext(&f->uc)) gf_die("fiber stack");
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = stack_bytes;
    f->uc.uc_link = NULL;
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))gf_entry, 2, (unsigned)(p & 0xFFFFFFFFu), (unsigned)(p >> 32));
#endif
    return f;
}

void* guest_fiber_state(guest_fiber* f) { return f->saved; }

void guest_fiber_switch(guest_fiber* to)
{
    guest_fiber* cur = t_cur;
    if (!cur || !to) gf_die("switch without a current or target fiber");
    if (to == cur) return;
    memcpy(cur->saved, cur->ctx, cur->ctx_size);
    memcpy(cur->ctx, to->saved, cur->ctx_size);
    for (int i = 0; i < g_nlocals; i++) {
        int* p = g_locals[i]();
        cur->locals[i] = *p;
        *p = to->locals[i];
    }
    t_cur = to;
#ifdef _WIN32
    SwitchToFiber(to->h);
#else
    if (swapcontext(&cur->uc, &to->uc)) gf_die("swapcontext");
#endif
    /* Resumed: whoever switched here already loaded our block into ctx (above) and set t_cur. */
    t_cur = cur;
}

void guest_fiber_destroy(guest_fiber* f)
{
    if (!f || f == t_cur || f->is_thread) gf_die("destroy of the running or thread fiber");
#ifdef _WIN32
    DeleteFiber(f->h);
#else
    free(f->stack);
#endif
    free(f->saved);
    free(f);
}

void guest_fiber_yield_thread(void)
{
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}

double guest_fiber_seconds(void)
{
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

struct gf_thread_arg { void (*fn)(void*); void* arg; };
#ifdef _WIN32
static DWORD WINAPI gf_thread_main(LPVOID p)
#else
static void* gf_thread_main(void* p)
#endif
{
    struct gf_thread_arg a = *(struct gf_thread_arg*)p;
    free(p);
    a.fn(a.arg);
    return 0;
}

int guest_fiber_spawn_thread(void (*fn)(void*), void* arg)
{
    struct gf_thread_arg* a = (struct gf_thread_arg*)malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn; a->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, gf_thread_main, a, 0, NULL);
    if (!h) { free(a); return -1; }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, gf_thread_main, a)) { free(a); return -1; }
    pthread_detach(t);
#endif
    return 0;
}

void guest_fiber_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
#endif
}
