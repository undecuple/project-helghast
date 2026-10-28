/* Spu pass 115 host test: why [fiber]/[heap] lines vanished from llvm-mingw builds, and that the port/spu stdio
 * redirect puts them in the log file. No game data.
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_r115_logroute.sh
 * The child does what kz3.exe does: kz3_log_init, then ps3_stdio_init's setvbuf(stderr, 64 KB, _IOFBF), prints one
 * line from a TU without the redirect (port/spu before pass 115) and lines from a TU with it, calls the flush
 * thread's fflush(stderr) from a redirected TU (= kz3_log_fflush; before the pass 115 follow-up a no-op for stderr,
 * which lost the raw line), and is killed (_exit, like a run ended from outside). Expected: the routed lines are in
 * the log file; the raw line is not in the log file but reaches stderr through that flush (it is the only flush the
 * non-redirected writers get under ps3_stdio's full buffering). */
#include "kz3_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

void r115_routed_print(void);
void r115_routed_flush(void);
void r115_raw_print(void);

static int file_has(const char* path, const char* needle)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s TMPDIR\n", argv[0]);
        return 2;
    }
    const char* dir = argv[1];
    char errpath[1024];
    snprintf(errpath, sizeof errpath, "%s/child_stderr.txt", dir);
    pid_t pid = fork();
    if (pid == 0) {
        if (!freopen(errpath, "w", stderr)) _exit(3);
        if (kz3_log_init(dir, KZ3_LOG_F_NO_ENV) != 0) _exit(4);
        static char ebuf[64 * 1024];
        setvbuf(stderr, ebuf, _IOFBF, sizeof ebuf); /* ps3_stdio_init (runtime/platform/ps3_stdio.c) */
        r115_raw_print();
        r115_routed_print();
        r115_routed_flush();
        _exit(0); /* killed: no atexit, no CRT flush */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("[FAIL] child status %d\n", st);
        return 1;
    }
    char logpath[1024] = "";
    char logs[1024];
    snprintf(logs, sizeof logs, "%s/logs", dir);
    DIR* d = opendir(logs);
    for (struct dirent* e; d && (e = readdir(d));)
        if (strstr(e->d_name, ".log")) snprintf(logpath, sizeof logpath, "%s/%s", logs, e->d_name);
    if (d) closedir(d);
    int fails = 0;
#define CHECK(c, msg)                                                                                                  \
    do {                                                                                                               \
        if (c)                                                                                                         \
            printf("[PASS] %s\n", msg);                                                                                \
        else {                                                                                                         \
            printf("[FAIL] %s\n", msg);                                                                                \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
    CHECK(logpath[0], "log file created");
    CHECK(file_has(logpath, "[fiber] S=0x114B7900 run_all #1 R115ROUTED") == 1,
          "redirected [fiber] S= run_all line is in the log file");
    CHECK(file_has(logpath, "R115OLDFMT") == 0,
          "old '[fiber] run_all' format routes at debug (why the format changed)");
    CHECK(file_has(logpath, "[heap] R115ROUTED2") == 1, "redirected [heap] line is in the log file");
    CHECK(file_has(logpath, "R115RAW") == 0, "raw stderr line is not in the log file (cause, part 1)");
    CHECK(file_has(errpath, "R115RAW") == 1,
          "raw stderr line reaches stderr: the redirected fflush flushes the CRT buffer before the kill (fix, part 2)");
    printf("%s\n", fails ? "FAIL" : "PASS: test_r115_logroute");
    return fails ? 1 : 0;
}
