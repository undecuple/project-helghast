/* host test for port/kz3/kz3_log.c (Linux, gcc). Run: tools/smoke/test_kz3_log.sh
 * Covers levels, per-tag filters and the KZ3_LOG spec, log rotation (newest 10), the 200-line ring, the fatal path
 * (crash file, hook, KZ3_CONTINUE_ON_FATAL, exit code without hook), thread safety (8 threads), the line router
 * (tag table, continuation lines, printf partial lines, filtered lines never formatted), the stdout echo format that
 * tools/kz3_metrics.py parses, once-per-key and the stdio redirect macros (test_kz3_log_redirect.c).
 * Every check has a negative control (marked NEG). Exit code 0 = all pass. argv[1] = scratch directory. */
#define _GNU_SOURCE 1
#include "kz3_log.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void redirect_tu_run(const char* root);   /* test_kz3_log_redirect.c */

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("[FAIL] %s:%d ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_root[512];

static char* slurp(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (!f) return strdup("");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = malloc((size_t)n + 1);
    size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f);
    return b;
}
static int has(const char* hay, const char* needle) { return strstr(hay, needle) != NULL; }
static int count_sub(const char* hay, const char* needle)
{
    int n = 0; for (const char* p = hay; (p = strstr(p, needle)); p++) n++; return n;
}
static int count_files(const char* dir, const char* pre, const char* suf)
{
    DIR* d = opendir(dir); struct dirent* e; int n = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name), lp = strlen(pre), ls = strlen(suf);
        if (l >= lp + ls && !strncmp(e->d_name, pre, lp) && !strcmp(e->d_name + l - ls, suf)) n++;
    }
    closedir(d);
    return n;
}
static void mkdirs(const char* sub, char* out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_root, sub);
    mkdir(out, 0755);
}
static char* current_log(void) { return slurp(kz3_log_file_path()); }

/* ------------------------------------------------------------------ levels + tag filters */
static void t_levels(void)
{
    char d[600]; mkdirs("levels", d, sizeof d);
    CHECK(kz3_log_init(d, KZ3_LOG_F_NO_ENV) == 0, "init");
    CHECK(kz3_log_file_path()[0] != 0, "log path set");
    kz3_log(KZ3_LOG_INFO, "lv", "info-line %d", 1);
    kz3_log(KZ3_LOG_WARN, "lv", "warn-line");
    kz3_log(KZ3_LOG_DEBUG, "lv", "debug-line-hidden");                 /* NEG: default threshold is info */
    KZ3_LOGT("lv", "trace-line-hidden");
    kz3_log_set_default_level(KZ3_LOG_TRACE);
    KZ3_LOGD("lv", "debug-line-shown");
    kz3_log_set_default_level(KZ3_LOG_WARN);
    KZ3_LOGI("lv", "info-line-hidden");                                /* NEG */
    KZ3_LOGE("lv", "error-line-shown");
    char* s = current_log();
    CHECK(has(s, " I [lv] info-line 1\n"), "info at info");
    CHECK(has(s, " W [lv] warn-line\n"), "warn at info");
    CHECK(!has(s, "debug-line-hidden") && !has(s, "trace-line-hidden"), "NEG debug/trace hidden at info");
    CHECK(has(s, " D [lv] debug-line-shown"), "debug at trace");
    CHECK(!has(s, "info-line-hidden"), "NEG info hidden at warn");
    CHECK(has(s, " E [lv] error-line-shown"), "error at warn");
    /* line format: ms, tid, level, [tag] */
    regex_t re; regcomp(&re, "^ *[0-9]+ +[0-9]+ W \\[lv\\] warn-line$", REG_EXTENDED | REG_NEWLINE);
    CHECK(regexec(&re, s, 0, NULL, 0) == 0, "line prefix format");
    regfree(&re);
    free(s);

    /* per-tag spec */
    kz3_log_set_default_level(KZ3_LOG_INFO);
    CHECK(kz3_log_configure("noisy:trace, [RSX null]:debug ,quiet:off") == 0, "configure ok");
    CHECK(kz3_log_configure("x:loud") == -1, "NEG bad level rejected");
    CHECK(kz3_log_threshold("RSX null", 8) == KZ3_LOG_DEBUG, "tag with space and brackets");
    CHECK(kz3_log_threshold("other", 5) == KZ3_LOG_INFO, "NEG unlisted tag keeps default");
    KZ3_LOGT("noisy", "noisy-trace-shown");
    KZ3_LOGE("quiet", "quiet-error-hidden");                           /* off silences even errors */
    KZ3_LOGD("other", "other-debug-hidden");                           /* NEG */
    CHECK(kz3_log_configure("*:error") == 0, "star default");
    KZ3_LOGW("other", "other-warn-hidden");
    KZ3_LOGT("noisy", "noisy-trace-still-shown");
    CHECK(kz3_log_parse_level("Warning", 7) == KZ3_LOG_WARN && kz3_log_parse_level("t", 1) == KZ3_LOG_TRACE &&
          kz3_log_parse_level("bogus", 5) == -2, "level names");
    s = current_log();
    CHECK(has(s, "noisy-trace-shown") && has(s, "noisy-trace-still-shown"), "override enables trace");
    CHECK(!has(s, "quiet-error-hidden"), "off silences");
    CHECK(!has(s, "other-debug-hidden") && !has(s, "other-warn-hidden"), "NEG others filtered");
    free(s);
    kz3_log_configure("*:info,noisy:info,RSX null:info,quiet:info");
    kz3_log_shutdown();

    /* env: KZ3_LOG_LEVEL + KZ3_LOG read at init unless NO_ENV */
    char d2[600]; mkdirs("levels-env", d2, sizeof d2);
    setenv("KZ3_LOG_LEVEL", "debug", 1); setenv("KZ3_LOG", "envtag:trace", 1);
    kz3_log_init(d2, 0);
    KZ3_LOGD("x", "env-debug-shown"); KZ3_LOGT("envtag", "env-trace-shown"); KZ3_LOGT("x", "env-trace-hidden");
    s = current_log();
    CHECK(has(s, "env-debug-shown") && has(s, "env-trace-shown"), "env levels applied");
    CHECK(!has(s, "env-trace-hidden"), "NEG env default is debug, not trace");
    free(s);
    kz3_log_shutdown();
    unsetenv("KZ3_LOG_LEVEL"); unsetenv("KZ3_LOG");
    kz3_log_configure("*:info,envtag:info");
}

/* ------------------------------------------------------------------ rotation */
static void t_rotation(void)
{
    char d[600], logs[700], p[800];
    mkdirs("rot", d, sizeof d);
    snprintf(logs, sizeof logs, "%s/logs", d); mkdir(logs, 0755);
    for (int i = 0; i < 12; i++) {
        snprintf(p, sizeof p, "%s/Killzone3-PC-200001%02d-000000.log", logs, i + 1);
        FILE* f = fopen(p, "w"); fputs("old\n", f); fclose(f);
    }
    snprintf(p, sizeof p, "%s/notes.txt", logs); fclose(fopen(p, "w"));
    snprintf(p, sizeof p, "%s/Killzone3-PC-setup-20000101-000000.log", logs); fclose(fopen(p, "w"));   /* not ours */
    snprintf(p, sizeof p, "%s/Killzone3-PC-keep.txt", logs); fclose(fopen(p, "w"));
    CHECK(count_files(logs, "Killzone3-PC-2", ".log") == 12, "12 old logs before");
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    kz3_log_shutdown();
    CHECK(count_files(logs, "Killzone3-PC-2", ".log") == 10, "10 logs kept after init: %d", count_files(logs, "Killzone3-PC-2", ".log"));
    snprintf(p, sizeof p, "%s/Killzone3-PC-20000103-000000.log", logs);
    CHECK(access(p, F_OK) != 0, "oldest (#3) pruned");
    snprintf(p, sizeof p, "%s/Killzone3-PC-20000104-000000.log", logs);
    CHECK(access(p, F_OK) == 0, "NEG #4 kept");
    snprintf(p, sizeof p, "%s/notes.txt", logs);
    CHECK(access(p, F_OK) == 0, "NEG other files untouched");
    snprintf(p, sizeof p, "%s/Killzone3-PC-keep.txt", logs);
    CHECK(access(p, F_OK) == 0, "NEG Killzone3-PC-*.txt untouched");
    snprintf(p, sizeof p, "%s/Killzone3-PC-setup-20000101-000000.log", logs);
    CHECK(access(p, F_OK) == 0, "NEG the setup's Killzone3-PC-setup-*.log untouched");
    /* a second start in the same second gets a distinct name, still 10 kept */
    kz3_log_init(d, KZ3_LOG_F_NO_ENV); char first[600]; snprintf(first, sizeof first, "%s", kz3_log_file_path());
    kz3_log_shutdown();
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    CHECK(strcmp(first, kz3_log_file_path()) != 0, "same-second restart gets a new file");
    kz3_log_shutdown();
    CHECK(count_files(logs, "Killzone3-PC-2", ".log") == 10, "still 10");
}

/* ------------------------------------------------------------------ ring */
static void t_ring(void)
{
    char d[600]; mkdirs("ring", d, sizeof d);
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    for (int i = 0; i < 250; i++) kz3_log(KZ3_LOG_INFO, "ring", "line-%03d|", i);
    KZ3_LOGD("ring", "ring-debug-hidden");                             /* NEG: filtered lines not in ring */
    size_t cap = 200 * 420; char* b = malloc(cap);
    kz3_log_ring_snapshot(b, cap);
    int lines = 0; for (char* p = b; *p; p++) lines += *p == '\n';
    CHECK(lines == 200, "ring holds 200 lines: %d", lines);
    CHECK(has(b, "line-249|") && has(b, "line-050|"), "newest 200 kept");
    CHECK(!has(b, "line-049|"), "NEG line 49 overwritten");
    CHECK(!has(b, "ring-debug-hidden"), "NEG filtered line absent");
    CHECK(strstr(b, "line-050|") < strstr(b, "line-249|"), "oldest first");
    free(b);
    kz3_log_shutdown();
}

/* ------------------------------------------------------------------ fatal */
static int g_hook_calls, g_hook_ret;
static char g_hook_msg[256], g_hook_path[600];
static int hook(const char* m, const char* path, void* u)
{
    (void)u; g_hook_calls++;
    snprintf(g_hook_msg, sizeof g_hook_msg, "%s", m); snprintf(g_hook_path, sizeof g_hook_path, "%s", path);
    return g_hook_ret;
}
static void t_fatal(void)
{
    char d[600], logs[700]; mkdirs("fatal", d, sizeof d);
    snprintf(logs, sizeof logs, "%s/logs", d);
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    KZ3_LOGI("pre", "context-before-fatal");
    CHECK(count_files(logs, "Killzone3-PC-crash-", ".txt") == 0, "NEG no crash file before a fatal");
    kz3_log_set_fatal_hook(hook, NULL);
    g_hook_ret = 1;
    unsetenv("KZ3_CONTINUE_ON_FATAL");
    kz3_log_fatal("HardExit", "Did not find object of type '%s'", "BulletSystemResource");
    CHECK(g_hook_calls == 1, "hook called once");
    CHECK(!strcmp(g_hook_msg, "Did not find object of type 'BulletSystemResource'"), "hook message");
    CHECK(count_files(logs, "Killzone3-PC-crash-", ".txt") == 1, "crash file written");
    char* c = slurp(g_hook_path);
    CHECK(has(c, "reason: fatal error") && has(c, "BulletSystemResource"), "crash file has the message");
    CHECK(has(c, "context-before-fatal"), "crash file has the ring");
    CHECK(has(c, "log file: ") && has(c, kz3_log_file_path()), "crash file names the log");
    free(c);
    char* s = current_log();
    CHECK(has(s, " E [HardExit] FATAL: Did not find"), "fatal logged as error");
    free(s);
    setenv("KZ3_CONTINUE_ON_FATAL", "1", 1);
    kz3_log_fatal("t", "second");
    CHECK(g_hook_calls == 1, "NEG hook not called with KZ3_CONTINUE_ON_FATAL=1");
    unsetenv("KZ3_CONTINUE_ON_FATAL");
    /* hook returning 0 / no hook: the process stops with KZ3_LOG_FATAL_EXIT (child processes) */
    for (int variant = 0; variant < 3; variant++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (variant == 0) { g_hook_ret = 0; kz3_log_set_fatal_hook(hook, NULL); }
            if (variant == 1) kz3_log_set_fatal_hook(NULL, NULL);
            if (variant == 2) { setenv("KZ3_CONTINUE_ON_FATAL", "1", 1); kz3_log_set_fatal_hook(NULL, NULL); }
            kz3_log_fatal("t", "child %d", variant);
            _exit(0);                                                 /* reached only when fatal returns */
        }
        int st = 0; waitpid(pid, &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (variant < 2) CHECK(code == KZ3_LOG_FATAL_EXIT, "fatal variant %d exits %d (got %d)", variant,
                               KZ3_LOG_FATAL_EXIT, code);
        else CHECK(code == 0, "NEG continue-on-fatal child returns from fatal (got %d)", code);
    }
    s = current_log();
    CHECK(has(s, "stopping (exit code 70)"), "stop logged");
    free(s);
    /* write_crash for exception filters: no hook */
    int before = g_hook_calls;
    const char* p = kz3_log_write_crash("unhandled exception 0xC0000005", "rip rva=0x1234");
    c = slurp(p);
    CHECK(has(c, "reason: unhandled exception 0xC0000005") && has(c, "rip rva=0x1234"), "write_crash content");
    CHECK(g_hook_calls == before, "NEG write_crash does not call the hook");
    free(c);
    kz3_log_set_fatal_hook(NULL, NULL);
    kz3_log_shutdown();
}

/* ------------------------------------------------------------------ threads */
#define NTHR 8
#define NPER 5000
static void* thr(void* a)
{
    int id = (int)(intptr_t)a;
    char lab[16]; snprintf(lab, sizeof lab, "w%d", id);
    kz3_log_set_thread_label(lab);
    for (int i = 0; i < NPER; i++) {
        if (i & 1) kz3_log(KZ3_LOG_INFO, "thr", "t=%d i=%d payload=ABCDEFGHIJKLMNOPQRSTUVWXYZ", id, i);
        else kz3_log_printf("[thr] t=%d i=%d payload=%s\n", id, i, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        if (i % 7 == 0) KZ3_LOGT("thr", "hidden %d", i);
    }
    return NULL;
}
/* returns 1 if every line of s is a well-formed thr line and each thread's i runs 0..NPER-1 in order */
static int check_thr_log(const char* s, int* nlines)
{
    int next[NTHR] = {0}; *nlines = 0;
    for (const char* p = s; *p;) {
        const char* e = strchr(p, '\n'); if (!e) return 0;
        if (strstr(p, "[thr]") && strstr(p, "[thr]") < e) {
            unsigned long long ms; unsigned long tid; char lab[16]; int t, i; char pay[40];
            int n = sscanf(p, "%llu %lu %15s I [thr] t=%d i=%d payload=%39s", &ms, &tid, lab, &t, &i, pay);
            if (n != 6 || t < 0 || t >= NTHR || i != next[t] || strcmp(pay, "ABCDEFGHIJKLMNOPQRSTUVWXYZ")) return 0;
            char want[16]; snprintf(want, sizeof want, "w%d", t);
            if (strcmp(lab, want)) return 0;
            next[t]++; (*nlines)++;
        }
        p = e + 1;
    }
    for (int t = 0; t < NTHR; t++) if (next[t] != NPER) return 0;
    return 1;
}
static void t_threads(void)
{
    char d[600]; mkdirs("threads", d, sizeof d);
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    pthread_t th[NTHR];
    for (int i = 0; i < NTHR; i++) pthread_create(&th[i], NULL, thr, (void*)(intptr_t)i);
    for (int i = 0; i < NTHR; i++) pthread_join(th[i], NULL);
    char* s = current_log();
    int n = 0;
    CHECK(check_thr_log(s, &n) && n == NTHR * NPER, "%d threads x %d lines intact and ordered (%d)", NTHR, NPER, n);
    CHECK(!has(s, "hidden"), "NEG filtered trace absent");
    free(s);
    /* NEG: the checker rejects an interleaved line like the ones in today's stdout */
    const char* bad = "     1     2 w0 I [thr] t=0 i=0 payload=ABCDEFGHIJKLMN[thr] t=1 OPQRSTUVWXYZ\n";
    CHECK(!check_thr_log(bad, &n), "NEG checker catches an interleaved line");
    kz3_log_shutdown();
}

/* ------------------------------------------------------------------ router */
static void t_router(void)
{
    char tag[40];
    struct { const char* line; int lvl; const char* tag; } k[] = {
        {"[mfc-list] ISSUE img=2 cmd=0x45", KZ3_LOG_TRACE, "mfc-list"},
        {"[spurs-pm] DMA cmd=0x20", KZ3_LOG_TRACE, "spurs-pm"},
        {"[RSX null] set_viewport(0,0)", KZ3_LOG_TRACE, "RSX null"},
        {"[RSX null] Window created: 1280x720", KZ3_LOG_INFO, "RSX null"},
        {"[cellSpurs] EventFlagSet ef=1", KZ3_LOG_TRACE, "cellSpurs"},
        {"[cellSpurs] Initialize \"HiPrio\"", KZ3_LOG_INFO, "cellSpurs"},
        {"[evt] port_send id=3", KZ3_LOG_TRACE, "evt"},
        {"[evt] port_create -> id=1", KZ3_LOG_DEBUG, "evt"},
        {"[THREAD 11] created", KZ3_LOG_DEBUG, "THREAD 11"},
        {"[null-call] guest called a NULL function pointer", KZ3_LOG_ERROR, "null-call"},
        {"[null-write] guest wrote32", KZ3_LOG_WARN, "null-write"},
        {"[hle] unresolved NID 0x12345678", KZ3_LOG_WARN, "hle"},
        {"[fs] open '/dev_bdvd/x'", KZ3_LOG_INFO, "fs"},
        {"[flip] 5 at 1234 ms", KZ3_LOG_INFO, "flip"},
        {"[brand-new-tag] hello", KZ3_LOG_INFO, "brand-new-tag"},
        {"no tag at all", KZ3_LOG_INFO, ""},
        {"[unterminated tag", KZ3_LOG_INFO, ""},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        int l = kz3_log_route(k[i].line, strlen(k[i].line), tag, sizeof tag);
        CHECK(l == k[i].lvl && !strcmp(tag, k[i].tag), "route '%s' -> %d '%s' (got %d '%s')", k[i].line, k[i].lvl,
              k[i].tag, l, tag);
    }
    CHECK(kz3_log_route("[RSX nullx] set_viewport", 24, tag, sizeof tag) == KZ3_LOG_INFO, "NEG near-miss tag is info");

    char d[600]; mkdirs("router", d, sizeof d);
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    /* printf path at the default (info) threshold */
    int r1 = kz3_log_printf("[mfc-list] RESUME img=%d tag=0 MFCHIDDEN\n", 2);
    kz3_log_printf("0x01548: 00000000 00000000 00002C24 CONTHIDDEN\n");   /* continuation inherits trace */
    kz3_log_printf("FFFFFF13 00000007 CONT2HIDDEN\n");
    int r2 = kz3_log_printf("[fs] open '%s'\n", "/dev_bdvd/PS3_GAME/x");
    kz3_log_printf("    detail of fs open SHOWN-CONT\n");                  /* continuation of an info line */
    kz3_log_printf("[flip] ");                                             /* partial line, completed later */
    kz3_log_printf("%d at %d ms\n", 7, 4321);
    kz3_log_printf("[spurs-pm] DMA cmd=0x%X", 0x40);                       /* filtered, no '\n' in format */
    kz3_log_printf(" lsa=0x%X SKIPHIDDEN", 0xD00);
    kz3_log_printf(" end\n");
    kz3_log_printf("Untagged message SHOWN-UNTAGGED\n");
    fprintf(stderr, "%s", "");                                             /* real stdio untouched here */
    kz3_log_fprintf(stderr, "[null-write] guest wrote32 0x0 to 0x00000000 by guest-fn=0x%08X -- DROPPED\n", 0x8C3D0);
    kz3_log_puts("[boot] puts-line");
    kz3_log_fputs("[boot] fputs-", stderr); kz3_log_fputs("line\n", stdout);
    kz3_log_printf("[live-draw] frame 1 LIVEHIDDEN\n");
    kz3_log_line("[cellGcmSys] SetUserHandler LINEHIDDEN\n[cellGcmSys] Init(cmdSize=0x10000) LINESHOWN\n");
    char* s = current_log();
    CHECK(r1 == 0 && r2 > 0, "filtered printf is not formatted (ret %d), shown one is (%d)", r1, r2);
    CHECK(!has(s, "MFCHIDDEN") && !has(s, "CONTHIDDEN") && !has(s, "CONT2HIDDEN"), "trace tag + continuation hidden");
    CHECK(has(s, " I [fs] open '/dev_bdvd/PS3_GAME/x'\n"), "fs shown");
    CHECK(has(s, " I     detail of fs open SHOWN-CONT\n"), "continuation of an info line shown");
    CHECK(has(s, " I [flip] 7 at 4321 ms\n"), "partial printf joined into one line");
    CHECK(!has(s, "SKIPHIDDEN") && !has(s, " end\n") && !has(s, "DMA cmd"), "filtered partial line fully skipped");
    CHECK(has(s, " I Untagged message SHOWN-UNTAGGED\n"), "untagged non-continuation line is info");
    CHECK(has(s, " W [null-write] guest wrote32"), "null-write is a warning");
    CHECK(has(s, " I [boot] puts-line\n") && has(s, " I [boot] fputs-line\n"), "puts/fputs routed");
    CHECK(!has(s, "LIVEHIDDEN") && !has(s, "LINEHIDDEN"), "debug/trace tags hidden");
    CHECK(has(s, "LINESHOWN"), "kz3_log_line splits lines");
    free(s);
    /* NEG: the same lines appear once their tags are enabled */
    kz3_log_configure("mfc-list:trace,spurs-pm:trace,live-draw:debug");
    kz3_log_printf("[mfc-list] RESUME img=%d MFCSHOWN\n", 2);
    kz3_log_printf("0x01548: 00000000 CONTSHOWN\n");
    kz3_log_printf("[spurs-pm] DMA cmd=0x%X", 0x40);
    kz3_log_printf(" lsa=0x%X SKIPSHOWN\n", 0xD00);
    kz3_log_printf("[live-draw] frame 2 LIVESHOWN\n");
    s = current_log();
    CHECK(has(s, " T [mfc-list] RESUME img=2 MFCSHOWN") && has(s, " T 0x01548: 00000000 CONTSHOWN"),
          "NEG enabled trace tag and its continuation shown");
    CHECK(has(s, " T [spurs-pm] DMA cmd=0x40 lsa=0xD00 SKIPSHOWN"), "NEG enabled partial line joined");
    CHECK(has(s, " D [live-draw] frame 2 LIVESHOWN"), "NEG enabled debug tag shown");
    free(s);
    kz3_log_configure("mfc-list:info,spurs-pm:info,live-draw:info");
    redirect_tu_run(g_root);
    s = current_log();
    CHECK(has(s, " I [redir] printf 42\n") && has(s, " W [hle] unresolved NID 0xDEADBEEF\n"),
          "redirect macros route printf/fprintf(stderr)");
    CHECK(has(s, " I [redir] puts\n") && has(s, " I [redir] fputs\n"), "redirect puts/fputs");
    CHECK(!has(s, "REDIRHIDDEN"), "NEG redirected trace line filtered");
    CHECK(!has(s, "to-a-real-file"), "NEG fprintf to another FILE* not routed");
    free(s);
    char rf[700]; snprintf(rf, sizeof rf, "%s/redir.txt", g_root);
    s = slurp(rf);
    CHECK(has(s, "to-a-real-file 7\n"), "fprintf to another FILE* reaches that file");
    free(s);
    CHECK(kz3_log_once_key("hle", 0x1234) == 1 && kz3_log_once_key("hle", 0x1234) == 0, "once per key");
    CHECK(kz3_log_once_key("hle", 0x1235) == 1 && kz3_log_once_key("null-write", 0x1234) == 1,
          "NEG other key / other tag fire again");
    for (int i = 0; i < 3; i++) KZ3_LOG_ONCE(KZ3_LOG_WARN, "once", "once-site-line");
    s = current_log();
    CHECK(count_sub(s, "once-site-line") == 1, "KZ3_LOG_ONCE logs once (%d)", count_sub(s, "once-site-line"));
    free(s);
    kz3_log_shutdown();
}

/* ------------------------------------------------------------------ stdout echo (kz3_metrics.py format) */
static void t_stdout(void)
{
    char d[600], out[700]; mkdirs("stdout", d, sizeof d);
    snprintf(out, sizeof out, "%s/stdout.txt", g_root);
    for (int mode = 0; mode < 2; mode++) {
        fflush(stdout);
        int save = dup(1);
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1); close(fd);
        kz3_log_init(d, KZ3_LOG_F_NO_ENV | (mode ? KZ3_LOG_F_STDOUT : 0));
        kz3_log_printf("[flip] %d at %d ms\n", 580, 16123);
        kz3_log_printf("[fs] open '%s'\n", "/dev_bdvd/a");
        kz3_log_printf("[mfc-list] hidden\n");
        kz3_log_shutdown();
        fflush(stdout);
        dup2(save, 1); close(save);
        char* s = slurp(out);
        regex_t re; regcomp(&re, "^\\[flip\\] ([0-9]+) at ([0-9]+) ms$", REG_EXTENDED | REG_NEWLINE);
        regex_t fs; regcomp(&fs, "^\\[fs\\] open '", REG_EXTENDED | REG_NEWLINE);
        if (mode) {
            CHECK(regexec(&re, s, 0, NULL, 0) == 0 && regexec(&fs, s, 0, NULL, 0) == 0,
                  "stdout echo keeps today's format (kz3_metrics.py regexes)");
            CHECK(!has(s, "hidden"), "NEG filtered lines not echoed");
        } else {
            CHECK(!has(s, "[flip]"), "NEG no stdout echo without the flag");
        }
        regfree(&re); regfree(&fs);
        free(s);
    }
}

/* ---- game hooks (pass 64): HardExit text, SetCrashDumpField fields, [link-miss], and the override glue macros */
typedef struct { uint64_t gpr[32]; uint64_t lr; } ppu_context;   /* the fields the macros use */
static uint8_t* vm_base;
static void func_0008AB70__lifted(ppu_context* ctx)               /* stands in for the lifted HardExit body */
{
    uint32_t sp = (uint32_t)ctx->gpr[1];
    memcpy(vm_base + sp - 0x1080 + 0x70, "macro-hardexit text\n", 21);
}
KZ3_HOOK_HARDEXIT(0008AB70)
static int g_link_ret;
static void func_000DDFF0__lifted(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)g_link_ret; }
KZ3_HOOK_LINK_RESOLVE(000DDFF0)
static void func_0008CF08__lifted(ppu_context* ctx) { (void)ctx; }
KZ3_HOOK_CRASHDUMP_FIELD(0008CF08)

static void t_hooks(void)
{
    char d[600]; mkdirs("hooks", d, sizeof d);
    kz3_log_init(d, KZ3_LOG_F_NO_ENV);
    unsetenv("KZ3_HARDEXIT");
    vm_base = calloc(1, 0x20000);
    const uint32_t sp = 0x18000, buf = sp - 0x1080 + 0x70;
    const char* msg = "Did not find object of type 'BulletSystemResource' named 'bulletsystem'\n";
    memcpy(vm_base + buf, msg, strlen(msg) + 1);
    /* HardExit v1.00 */
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, sp, 0x0032BFA4, vm_base) == 1, "hardexit v1.00 logged");
    char* s = current_log();
    CHECK(has(s, " E [HardExit] Did not find object of type 'BulletSystemResource' named 'bulletsystem' "
                 "(caller lr=0x0032BFA4, HardExit @0x0008AB70, game 1.00)"), "hardexit line (newline stripped)");
    free(s);
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, sp, 0x0032BFA4, vm_base) == 0, "NEG same lr+text once");
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_114, sp, 0x0033AB10, vm_base) == 1, "hardexit 1.14 same frame");
    CHECK(kz3_log_hardexit(0x00001234u, sp, 0x0033AB20, vm_base) == 0, "NEG unknown address");
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, sp, 0x0032BFA4, NULL) == 0, "NEG no vm");
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, 0x1C000, 0x0032BFA4, vm_base) == 0, "NEG empty message");
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, 0x1000, 0x0032BFA4, vm_base) == 0, "NEG frame below 0x10000");
    s = current_log();
    CHECK(count_sub(s, "[HardExit]") == 2, "two HardExit lines");
    CHECK(has(s, "HardExit @0x0008C360, game 1.14"), "1.14 line names its site");
    free(s);
    /* KZ3_HARDEXIT=fatal goes through kz3_log_fatal (hook set by t_fatal, returns 1) */
    kz3_log_set_fatal_hook(hook, NULL); g_hook_calls = 0; g_hook_ret = 1;
    setenv("KZ3_HARDEXIT", "fatal", 1);
    CHECK(kz3_log_hardexit(KZ3_ADDR_HARDEXIT_100, sp, 0x0032BFB0, vm_base) == 1, "fatal mode logged");
    CHECK(g_hook_calls == 1 && has(g_hook_msg, "named 'bulletsystem'"), "fatal mode called the hook with the text");
    unsetenv("KZ3_HARDEXIT");
    /* the glue macro: reads the frame after the body */
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0x1A000; ctx.lr = 0x00401000;
    func_0008AB70(&ctx);
    s = current_log();
    CHECK(has(s, "[HardExit] macro-hardexit text (caller lr=0x00401000, HardExit @0x0008AB70"), "macro glue");
    free(s);
    /* SetCrashDumpField: field 10 feeds [link-miss], field 3 is silent, others at debug */
    uint8_t key[16] = {0x00,0x11,0x22,0x33, 0x44,0x55,0x66,0x77, 0x88,0x99,0xAA,0xBB, 0xCC,0xDD,0xEE,0xFF};
    CHECK(kz3_log_link_miss(0, key, 0x40001000, 0x000E6800) == 0, "NEG status 0 is silent");
    CHECK(kz3_log_link_miss(8, key, 0x40001000, 0x000E6800) == 1, "link-miss before any lump");
    CHECK(!strcmp(kz3_log_streaming_lump(), ""), "no lump yet");
    kz3_log_crashdump_field(10, (const uint8_t*)"lump_sp_default\n", 16);
    CHECK(!strcmp(kz3_log_streaming_lump(), "lump_sp_default"), "field 10 kept");
    kz3_log_crashdump_field(3, (const uint8_t*)"HardExit text via field 3", 25);
    kz3_log_crashdump_field(1, (const uint8_t*)"sp_corinth_01", 13);          /* hidden: default threshold is info */
    kz3_log_configure("crashdump:debug");
    kz3_log_crashdump_field(1, (const uint8_t*)"sp_corinth_02", 13);
    kz3_log_crashdump_field(99, (const uint8_t*)"x", 1);
    key[15] = 0xF0;
    CHECK(kz3_log_link_miss(8, key, 0x40002000, 0x000E6804) == 1, "link-miss with lump");
    CHECK(kz3_log_link_miss(8, key, 0x40002000, 0x000E6804) == 0, "NEG same target once");
    s = current_log();
    CHECK(has(s, " W [link-miss] lump='?' target=00112233445566778899AABBCCDDEEFF holder=0x40001000 lr=0x000E6800"),
          "link-miss line (words in memory order)");
    CHECK(has(s, "[link-miss] lump='lump_sp_default' target=00112233445566778899AABBCCDDEEF0 holder=0x40002000"),
          "link-miss names the lump");
    CHECK(count_sub(s, "[link-miss]") == 2, "two link-miss lines");
    CHECK(!has(s, "via field 3"), "NEG field 3 not logged by the field hook");
    CHECK(!has(s, "sp_corinth_01") && has(s, " D [crashdump] Level='sp_corinth_02'"), "other fields at debug only");
    CHECK(has(s, "[crashdump] field99='x'"), "unknown field id");
    free(s);
    /* glue macros for the link and field hooks */
    memcpy(vm_base + 0x11000, key, 16); vm_base[0x1100F] = 0xE0;
    memset(&ctx, 0, sizeof ctx); ctx.gpr[5] = 0x11000; ctx.gpr[6] = 0x40003000; ctx.lr = 0x000E6808;
    g_link_ret = 0; func_000DDFF0(&ctx);
    g_link_ret = 8; func_000DDFF0(&ctx);
    memcpy(vm_base + 0x12000, "lump_mp_default", 16);
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 10; ctx.gpr[4] = 0x12000; ctx.gpr[5] = 15;
    func_0008CF08(&ctx);
    s = current_log();
    CHECK(count_sub(s, "[link-miss]") == 3 && has(s, "target=00112233445566778899AABBCCDDEEE0 holder=0x40003000 lr=0x000E6808"),
          "link macro: one line for status 8, none for status 0");
    CHECK(!strcmp(kz3_log_streaming_lump(), "lump_mp_default"), "field macro");
    free(s);
    /* the per-run cap: KZ3_LINK_MISS_MAX lines, then one note */
    for (int i = 0; i < KZ3_LINK_MISS_MAX + 10; i++) { key[0] = (uint8_t)(i + 1); key[1] = (uint8_t)(i >> 8); kz3_log_link_miss(8, key, 1, 2); }
    s = current_log();
    CHECK(count_sub(s, " target=") == KZ3_LINK_MISS_MAX, "link-miss capped");
    CHECK(count_sub(s, "further [link-miss] lines suppressed") == 1, "one suppression note");
    free(s);
    free(vm_base); vm_base = NULL;
}

int main(int argc, char** argv)
{
    snprintf(g_root, sizeof g_root, "%s", argc > 1 ? argv[1] : "build/smoke-host/kz3_log");
    mkdir(g_root, 0755);
    t_levels();
    t_rotation();
    t_ring();
    t_fatal();
    t_threads();
    t_router();
    t_stdout();
    t_hooks();
    printf("test_kz3_log: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
