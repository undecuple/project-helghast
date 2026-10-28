/* Host test for KZ3 patch 2048 (asynchronous PSO builds, LD_PSO_ASYNC): the platform-neutral bookkeeping in the
 * patched toolkit's libs/video/rsx_pso_async.h. No game data, no GPU, no Windows: plain C + pthreads.
 *  1. Pending entries: a lookup of a pending key returns NULL (draw skipped); RSX_APSO_MERGE stores the PSO into
 *     the key's pending entry only (a failed build stores NULL), and reports no entry for a key with none pending.
 *  2. In-flight compile table: same source = one owner + dependents (deduped), a hash/length collision with another
 *     source gets its own entry, the entry goes with its last reference and hands back the table's blob.
 *  3. The worker protocol of rsx_live_draw.c (FIFO queue, owners compile and publish, dependents wait) with 1 and
 *     3 workers and 64 jobs over 6 VS and 5 PS sources: exactly one compile per source, every job gets its source's
 *     blob, a failing source gives NULL to all its jobs, no deadlock, and every fake blob reference is released.
 * Run by tools/smoke/test_pso_async.sh. */
#include "rsx_pso_async.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_fail = 1;                                                                                                \
            printf("[FAIL] " __VA_ARGS__);                                                                             \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

typedef struct {
    uint64_t key;
    void* pso;
    uint8_t preloaded, pending;
} entry_t; /* psocache_t's fields */
static void* lookup(entry_t* a, uint32_t n, uint64_t key, int* found, uint64_t* skipped)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i].key == key) {
            *found = 1;
            if (a[i].pending) (*skipped)++;
            return a[i].pso;
        }
    *found = 0;
    return NULL;
}

static void test_pending(void)
{
    entry_t a[8];
    uint32_t n = 0;
    uint64_t skipped = 0;
    int found = 0, ok = 0;
    int pso1, pso2;
    a[n++] = (entry_t){10, &pso1, 1, 0}; /* a preloaded PSO */
    a[n++] = (entry_t){20, NULL, 0, 1};  /* async build in flight */
    a[n++] = (entry_t){30, NULL, 0, 1};
    CHECK(lookup(a, n, 20, &found, &skipped) == NULL && found && skipped == 1, "pending lookup must skip the draw");
    CHECK(lookup(a, n, 20, &found, &skipped) == NULL && skipped == 2, "every draw of a pending key is skipped");
    RSX_APSO_MERGE(a, n, 20, (void*)&pso2, ok);
    CHECK(ok && a[1].pso == &pso2 && !a[1].pending, "merge stores the PSO and clears pending");
    CHECK(lookup(a, n, 20, &found, &skipped) == &pso2 && skipped == 2, "merged key draws");
    RSX_APSO_MERGE(a, n, 20, (void*)&pso1, ok);
    CHECK(!ok && a[1].pso == &pso2, "a second merge of the key finds no pending entry (caller releases)");
    RSX_APSO_MERGE(a, n, 10, (void*)&pso2, ok);
    CHECK(!ok && a[0].pso == &pso1, "a non-pending entry with the key is never overwritten");
    RSX_APSO_MERGE(a, n, 30, NULL, ok);
    CHECK(ok && a[2].pso == NULL && !a[2].pending, "a failed build merges as NULL (as a synchronous failure)");
    CHECK(lookup(a, n, 30, &found, &skipped) == NULL && skipped == 2, "failed key: NULL, not counted as pending");
    RSX_APSO_MERGE(a, n, 99, (void*)&pso1, ok);
    CHECK(!ok, "unknown key: no entry");
}

static void test_table(void)
{
    rsx_apso_blobtab t = {0};
    int own = -1;
    void* rel = (void*)1;
    int blob;
    const char* s1 = "float4 main() : SV_Target { return 1; }";
    const char* s2 = "float4 main() : SV_Target { return 2; }"; /* same length; forced same hash below */
    rsx_apso_blob* e1 = rsx_apso_blob_acquire(&t, 'P', s1, (uint32_t)strlen(s1), 7, &own);
    CHECK(e1 && own == 1 && t.count == 1 && t.owned == 1, "first acquire owns the compile");
    rsx_apso_blob* e2 = rsx_apso_blob_acquire(&t, 'P', s1, (uint32_t)strlen(s1), 7, &own);
    CHECK(e2 == e1 && own == 0 && t.deduped == 1 && e1->refs == 2, "same source depends on the owner");
    rsx_apso_blob* e3 = rsx_apso_blob_acquire(&t, 'P', s2, (uint32_t)strlen(s2), 7, &own);
    CHECK(e3 && e3 != e1 && own == 1 && t.count == 2, "hash collision with another source: own entry");
    rsx_apso_blob* e4 = rsx_apso_blob_acquire(&t, 'V', s1, (uint32_t)strlen(s1), 7, &own);
    CHECK(e4 && e4 != e1 && own == 1 && t.count == 3, "same text, other stage: own entry");
    rsx_apso_blob_publish(e1, &blob);
    CHECK(e1->done && e1->blob == &blob, "publish");
    CHECK(rsx_apso_blob_release(&t, e1, &rel) == 0 && rel == NULL && t.count == 3, "dependent release keeps the entry");
    CHECK(rsx_apso_blob_release(&t, e1, &rel) == 1 && rel == &blob && t.count == 2,
          "last release removes, hands the blob");
    rsx_apso_blob_publish(e3, NULL);
    CHECK(rsx_apso_blob_release(&t, e3, &rel) == 1 && rel == NULL && t.count == 1, "failed compile: NULL blob");
    CHECK(rsx_apso_blob_release(&t, NULL, &rel) == 0, "release of no entry");
    e2 = rsx_apso_blob_acquire(&t, 'P', s1, (uint32_t)strlen(s1), 7, &own);
    CHECK(e2 && own == 1, "after removal the next job owns a new compile (walker finds the blob in its cache first)");
    rsx_apso_blob_publish(e2, NULL);
    rsx_apso_blob_release(&t, e2, &rel);
    rsx_apso_blob_publish(e4, NULL);
    rsx_apso_blob_release(&t, e4, &rel);
    CHECK(t.count == 0 && t.head == NULL, "table empty");
}

/* --- 3: the worker protocol with fake refcounted blobs --- */
enum { NJOBS = 64, NVS = 6, NPS = 5 };
typedef struct {
    int refs, src;
} fblob;
static const char* g_src[NVS + NPS];
static int g_compiles[NVS + NPS], g_live_blobs;
typedef struct job {
    struct job* next;
    int vs_i, ps_i;
    fblob *vs, *ps;
    rsx_apso_blob *vs_e, *ps_e;
    int vs_own, ps_own;
} job;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, qcv = PTHREAD_COND_INITIALIZER;
static job *q_head, *q_tail;
static int q_closed, g_done_jobs;

static fblob* compile(int si, rsx_apso_blob* e)
{
    usleep(2000 + (si % 3) * 1500);
    pthread_mutex_lock(&mu);
    g_compiles[si]++;
    pthread_mutex_unlock(&mu);
    fblob* b = NULL;
    if (si != NVS + 2) {
        b = (fblob*)malloc(sizeof *b);
        b->refs = 1;
        b->src = si; /* PS #2 fails to compile */
        pthread_mutex_lock(&mu);
        g_live_blobs++;
        pthread_mutex_unlock(&mu);
    }
    if (e) {
        pthread_mutex_lock(&mu);
        if (b) b->refs++;
        rsx_apso_blob_publish(e, b);
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    return b;
}
static fblob* wait_blob(rsx_apso_blob* e)
{
    pthread_mutex_lock(&mu);
    while (!e->done) pthread_cond_wait(&cv, &mu);
    fblob* b = (fblob*)e->blob;
    if (b) b->refs++;
    pthread_mutex_unlock(&mu);
    return b;
}
static void unref(fblob* b)
{
    if (b && --b->refs == 0) {
        free(b);
        g_live_blobs--;
    }
} /* walker thread only */

static void* worker(void* arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!q_head && !q_closed) pthread_cond_wait(&qcv, &mu);
        job* j = q_head;
        if (j) {
            q_head = j->next;
            if (!q_head) q_tail = NULL;
        }
        pthread_mutex_unlock(&mu);
        if (!j) return NULL;
        if (j->vs_own || !j->vs_e) j->vs = compile(j->vs_i, j->vs_e);
        if (j->ps_own || !j->ps_e) j->ps = compile(NVS + j->ps_i, j->ps_e);
        if (!j->vs_own && j->vs_e) j->vs = wait_blob(j->vs_e);
        if (!j->ps_own && j->ps_e) j->ps = wait_blob(j->ps_e);
        pthread_mutex_lock(&mu);
        g_done_jobs++;
        pthread_mutex_unlock(&mu);
    }
}

static void test_workers(int nthreads)
{
    rsx_apso_blobtab t = {0};
    job jobs[NJOBS];
    pthread_t th[8];
    memset(g_compiles, 0, sizeof g_compiles);
    g_live_blobs = 0;
    q_head = q_tail = NULL;
    q_closed = 0;
    g_done_jobs = 0;
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < NJOBS; i++) { /* walker: register and queue while the workers run */
        job* j = &jobs[i];
        memset(j, 0, sizeof *j);
        j->vs_i = (i * 7) % NVS;
        j->ps_i = (i * 3 + i / 5) % NPS;
        const char* vs = g_src[j->vs_i];
        const char* ps = g_src[NVS + j->ps_i];
        j->vs_e = rsx_apso_blob_acquire(&t, 'V', vs, (uint32_t)strlen(vs), 1, &j->vs_own); /* one hash: collisions */
        j->ps_e = rsx_apso_blob_acquire(&t, 'P', ps, (uint32_t)strlen(ps), 1, &j->ps_own);
        pthread_mutex_lock(&mu);
        if (q_tail)
            q_tail->next = j;
        else
            q_head = j;
        q_tail = j;
        pthread_cond_signal(&qcv);
        pthread_mutex_unlock(&mu);
        if (i % 9 == 0) usleep(1000);
    }
    pthread_mutex_lock(&mu);
    q_closed = 1;
    pthread_cond_broadcast(&qcv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    CHECK(g_done_jobs == NJOBS, "%d threads: %d of %d jobs done", nthreads, g_done_jobs, NJOBS);
    int multi = 0;
    for (int s = 0; s < NVS + NPS; s++) multi |= g_compiles[s] != 1;
    CHECK(!multi, "%d threads: every source compiled exactly once", nthreads);
    CHECK(t.owned == NVS + NPS && t.deduped == 2 * NJOBS - (NVS + NPS), "%d threads: owned %llu deduped %llu", nthreads,
          (unsigned long long)t.owned, (unsigned long long)t.deduped);
    int bad = 0;
    for (int i = 0; i < NJOBS; i++) {
        bad |= !jobs[i].vs || jobs[i].vs->src != jobs[i].vs_i;
        bad |= (jobs[i].ps_i == 2) ? jobs[i].ps != NULL : (!jobs[i].ps || jobs[i].ps->src != NVS + jobs[i].ps_i);
    }
    CHECK(!bad, "%d threads: every job has its source's blob (NULL for the failing one)", nthreads);
    for (int i = NJOBS - 1; i >= 0; i--) { /* walker merges, in reverse order (owners last) */
        void* rel = NULL;
        if (rsx_apso_blob_release(&t, jobs[i].vs_e, &rel)) unref((fblob*)rel);
        if (rsx_apso_blob_release(&t, jobs[i].ps_e, &rel)) unref((fblob*)rel);
        unref(jobs[i].vs);
        unref(jobs[i].ps);
    }
    CHECK(t.count == 0 && g_live_blobs == 0, "%d threads: table empty (%u), blobs all released (%d live)", nthreads,
          t.count, g_live_blobs);
}

int main(void)
{
    static char srcs[NVS + NPS][64];
    for (int s = 0; s < NVS + NPS; s++) {
        snprintf(srcs[s], sizeof srcs[s], "shader source %02d padded", s);
        g_src[s] = srcs[s];
    }
    alarm(60); /* a deadlock fails the test instead of hanging the harness */
    test_pending();
    test_table();
    test_workers(1);
    test_workers(3);
    printf("[test_pso_async] %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
