/* Patch 2188 (O0): host-only microbenchmark of cross-thread wake latency, event vs spin. A waker timestamps and signals;
 * the sleeper records the time it resumes. Prints median / p99 / max in microseconds. Built for the host (pthreads) and,
 * with MinGW, for Windows (Win32 events; run it under Proton to see the Proton wake path: tools/proton-test.sh).
 * Not a test: it asserts nothing, the numbers go to the report. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
static double now_us(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1e6 / (double)f.QuadPart; }
static void pause_us(double us) { const double t = now_us(); while (now_us() - t < us) { } }
typedef HANDLE ev_t;
static void ev_init(ev_t* e) { *e = CreateEventA(NULL, FALSE, FALSE, NULL); }
static void ev_set(ev_t* e) { SetEvent(*e); }
static void ev_wait(ev_t* e) { WaitForSingleObject(*e, INFINITE); }
typedef HANDLE thr_t;
static DWORD WINAPI wrap(LPVOID p);
#else
#include <pthread.h>
#include <time.h>
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3; }
static void pause_us(double us) { const double t = now_us(); while (now_us() - t < us) { } }
typedef struct { pthread_mutex_t m; pthread_cond_t c; int set; } ev_t;
static void ev_init(ev_t* e) { pthread_mutex_init(&e->m, 0); pthread_cond_init(&e->c, 0); e->set = 0; }
static void ev_set(ev_t* e) { pthread_mutex_lock(&e->m); e->set = 1; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m); }
static void ev_wait(ev_t* e) { pthread_mutex_lock(&e->m); while (!e->set) pthread_cond_wait(&e->c, &e->m); e->set = 0; pthread_mutex_unlock(&e->m); }
typedef pthread_t thr_t;
#endif
enum { N = 20000 };
static ev_t g_ev; static atomic_int g_spin; static atomic_int g_stop; static double g_t_sig; static double g_lat[N]; static int g_mode;
static void sleeper(void)
{
    for (int i = 0; i < N; i++) {
        if (g_mode == 0) ev_wait(&g_ev); else while (!atomic_exchange_explicit(&g_spin, 0, memory_order_acquire)) { }
        g_lat[i] = now_us() - g_t_sig;
        atomic_store(&g_stop, i + 1);
    }
}
#ifdef _WIN32
static DWORD WINAPI wrap(LPVOID p) { (void)p; sleeper(); return 0; }
#else
static void* wrap(void* p) { (void)p; sleeper(); return 0; }
#endif
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static void run(int mode)
{
    g_mode = mode; atomic_store(&g_stop, 0); atomic_store(&g_spin, 0); ev_init(&g_ev);
    thr_t t;
#ifdef _WIN32
    t = CreateThread(NULL, 0, wrap, NULL, 0, NULL);
#else
    pthread_create(&t, 0, wrap, 0);
#endif
    for (int i = 0; i < N; i++) {
        pause_us(mode == 0 ? 60.0 : 30.0);   /* give the sleeper time to go to sleep / start spinning */
        g_t_sig = now_us();
        if (mode == 0) ev_set(&g_ev); else atomic_store_explicit(&g_spin, 1, memory_order_release);
        while (atomic_load(&g_stop) < i + 1) { }
    }
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
#else
    pthread_join(t, 0);
#endif
    qsort(g_lat, N, sizeof g_lat[0], cmp);
    printf("[wake-bench] %-5s n=%d median=%.1fus p99=%.1fus p99.9=%.1fus max=%.1fus\n", mode == 0 ? "event" : "spin", N, g_lat[N / 2], g_lat[N * 99 / 100], g_lat[N * 999 / 1000], g_lat[N - 1]);
}
int main(void) { run(0); run(1); return 0; }
