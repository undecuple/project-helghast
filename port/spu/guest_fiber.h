/* Host coroutines for guest context switches in statically recompiled code (upstream candidate).
 *
 * A lifted guest routine that saves r1/LR and later "returns" into another saved context (setjmp/longjmp-style
 * fiber switches) cannot run as plain C: the host stack and the host return address belong to whoever called
 * the lifted function, not to the guest context being resumed. This helper gives each guest fiber its own host
 * coroutine (Windows fibers; ucontext elsewhere) and keeps one shared guest register block consistent: the
 * lifted code of a host thread always uses the same context pointer, so a switch stores that block into the
 * fiber being left and loads the block of the fiber being resumed.
 *
 * Usage: guest_fiber_self() once on the scheduling thread, guest_fiber_create() for each guest fiber (its
 * initial register block is a copy of the current one; edit it through guest_fiber_state()), then
 * guest_fiber_switch(). A body that returns switches back to the fiber that created it. Not thread-safe across
 * host threads: a fiber runs only on the host thread that created it. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct guest_fiber guest_fiber;
typedef void (*guest_fiber_body)(void* ctx, void* arg);

/* The calling host thread as a fiber (created on first use). ctx = the guest register block the lifted code of
 * this thread uses, ctx_size its size. */
guest_fiber* guest_fiber_self(void* ctx, size_t ctx_size);
/* A new suspended fiber that runs body(ctx, arg) when first switched to. stack_bytes 0 = 32 MiB reserve. */
guest_fiber* guest_fiber_create(guest_fiber_body body, void* arg, size_t stack_bytes);
/* The register block `to` starts or resumes with (valid while `to` is suspended). */
void* guest_fiber_state(guest_fiber* f);
/* Save the shared block into the current fiber, switch to `to`, and reload the block when switched back. */
void guest_fiber_switch(guest_fiber* to);
guest_fiber* guest_fiber_current(void);
/* Per-fiber host thread-locals: up to 4 ints (given as getters of this thread's slot) that each fiber keeps its own
 * value of, saved and restored at every switch; a new fiber starts at 0. For counters that live in host frames
 * (a runtime's call-depth guard): a fiber that ends by switching away from inside its frames never unwinds them,
 * so a shared counter would only grow. Register once per process before the first switch. Returns 0 on success. */
int guest_fiber_add_local(int* (*slot)(void));
/* Free a suspended (or finished) fiber that is not the current one. Its host stack is dropped without unwinding. */
void guest_fiber_destroy(guest_fiber* f);
/* Small host helpers for schedulers that spin on guest memory: give up the time slice; monotonic seconds. */
void guest_fiber_yield_thread(void);
double guest_fiber_seconds(void);
/* A detached host thread (diagnostic watchdogs); 0 on success. And a plain sleep. */
int guest_fiber_spawn_thread(void (*fn)(void*), void* arg);
void guest_fiber_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif
