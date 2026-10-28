/* test_crash_dump.c: the crash handler's minidump step, without the game (RSX pass 33).
 *
 * Pass 31/32 open item: in kz3.exe under Proton, nothing after MiniDumpWriteDump in ppu_crash_report (patch 2030)
 * ran: the "[CRASH] minidump ..." line never appeared, kz3_crash_notify was not reached, the process ended with
 * 0xC0000005. This exe reproduces the handler's sequence on its own (kz3_log + an unhandled-exception filter + a
 * forced access violation on a worker thread) in two variants, each in a child process:
 *
 *   bug   the round-32 sequence: dbghelp loaded inside the filter, a hand-rolled, naturally aligned
 *         `{ DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; }` passed as MINIDUMP_EXCEPTION_INFORMATION,
 *         MiniDumpWriteDump on the faulting thread. Expected under Proton: a nested access violation inside dbghelp
 *         (dump_exception_info reads ExceptionPointers at +4, the SDK's 4-byte packing; the struct has it at +8),
 *         exit 0xC0000005, no "filter done".
 *   fix   kz3_log_write_minidump (port/kz3/kz3_log.c): the header's packed type, dbghelp resolved at init, dump on
 *         a helper thread. Expected: "[CRASH] minidump '...dmp'", "filter done", exit 70.
 *
 * A first-position vectored handler in each child logs every exception (code, address, module) so the nested fault
 * inside dbghelp shows up as evidence, and the filter logs the free stack at entry. The parent (`--all`) runs both
 * variants, prints one line per variant with the exit code and the dump size, and exits 0 when "fix" completed with
 * a dump (the "bug" line is informational: real Windows may read different garbage).
 *
 * Build: tools/smoke/build-host.sh (build/smoke/crash/test_crash_dump.exe, its own directory so proton-test.sh
 * copies only it). Run: nix develop --command tools/proton-test.sh --seconds 40 --name rsx33-crash \
 *      build/smoke/crash/test_crash_dump.exe --all     (no window, no GPU lock needed).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_log.h"

static const char* g_variant = "bug";
static LONG g_veh_lines;

static int has(const char* s)
{
    return strstr(g_variant, s) != NULL;
}

/* ---------------------------------------------------------------------- nested-exception evidence */
static LONG WINAPI veh_trace(EXCEPTION_POINTERS* ep)
{
    static __thread int in;
    if (in) return EXCEPTION_CONTINUE_SEARCH;
    in = 1;
    if (InterlockedIncrement(&g_veh_lines) <= 8) {
        void* a = ep->ExceptionRecord->ExceptionAddress;
        HMODULE m = NULL;
        char name[260] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)a, &m) &&
            m)
            GetModuleFileNameA(m, name, sizeof name);
        kz3_log(
            KZ3_LOG_ERROR, "veh", "exception 0x%08lX at %p (%s+0x%llX) tid=%lu param0=%llu param1=0x%llX",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, a, name, (unsigned long long)((char*)a - (char*)m),
            (unsigned long)GetCurrentThreadId(),
            ep->ExceptionRecord->NumberParameters > 0 ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0]
                                                      : 0ull,
            ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]
                                                      : 0ull);
    }
    in = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------------------------------------------------------------- the round-32 dump (bug) */
typedef BOOL(WINAPI* mdw_fn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
static int dump_bug(EXCEPTION_POINTERS* ep, const char* path)
{
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    mdw_fn mdw = dh ? (mdw_fn)(void*)GetProcAddress(dh, "MiniDumpWriteDump") : NULL;
    char dll[260] = "?";
    if (dh) GetModuleFileNameA(dh, dll, sizeof dll);
    kz3_log(KZ3_LOG_INFO, "test", "dbghelp %p (%s) MiniDumpWriteDump %p, dumping on tid %lu", (void*)dh, dll,
            (void*)mdw, (unsigned long)GetCurrentThreadId());
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    struct {
        DWORD tid;
        EXCEPTION_POINTERS* ep;
        BOOL client;
    } mei = {GetCurrentThreadId(), ep, FALSE}; /* ep at +8 */
    BOOL ok =
        mdw ? mdw(GetCurrentProcess(), GetCurrentProcessId(), f, 0 /* MiniDumpNormal */, &mei, NULL, NULL) : FALSE;
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    kz3_log(KZ3_LOG_INFO, "test", "MiniDumpWriteDump returned %d (err %lu)", (int)ok, (unsigned long)err);
    return ok ? 1 : 0;
}

static LONG WINAPI filter(EXCEPTION_POINTERS* ep)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    char rsp_probe;
    kz3_log(KZ3_LOG_ERROR, "CRASH", "code=0x%08lX tid=%lu free stack %lld KB (variant %s)",
            (unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned long)GetCurrentThreadId(),
            (long long)(&rsp_probe - (char*)tib->StackLimit) / 1024, g_variant);
    char details[128];
    snprintf(details, sizeof details, "rip=%p", ep->ExceptionRecord->ExceptionAddress);
    const char* crash = kz3_log_write_crash("unhandled exception (test)", details);
    kz3_log(KZ3_LOG_ERROR, "CRASH", "crash file '%s'", crash);
    if (crash && *crash) {
        int ok;
        if (has("fix"))
            ok = kz3_log_write_minidump(crash, ep, (unsigned long)GetCurrentThreadId());
        else {
            char dmp[600];
            snprintf(dmp, sizeof dmp, "%s", crash);
            char* dot = strrchr(dmp, '.');
            if (dot) strcpy(dot, ".dmp");
            ok = dump_bug(ep, dmp);
            if (!ok) DeleteFileA(dmp);
        }
        kz3_log(KZ3_LOG_ERROR, "CRASH", "minidump %s", ok ? "written" : "NOT written");
    }
    kz3_log(KZ3_LOG_ERROR, "test", "filter done");
    kz3_log_shutdown();
    TerminateProcess(GetCurrentProcess(), 70);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI faulter(LPVOID p)
{
    (void)p;
    Sleep(300);
    kz3_log_set_thread_label("test");
    kz3_log(KZ3_LOG_ERROR, "test", "forcing a host access violation");
    *(volatile int*)(uintptr_t)8 = 1;
    return 0;
}

static int child(const char* variant)
{
    g_variant = variant;
    kz3_log_init(".", KZ3_LOG_F_STDOUT | KZ3_LOG_F_NO_ENV);
    kz3_log_set_thread_label("main");
    kz3_log(KZ3_LOG_INFO, "test", "child variant %s pid %lu", variant, (unsigned long)GetCurrentProcessId());
    AddVectoredExceptionHandler(1, veh_trace);
    SetUnhandledExceptionFilter(filter);
    HANDLE t = CreateThread(NULL, 0, faulter, NULL, 0, NULL);
    WaitForSingleObject(t, 60000);
    kz3_log(KZ3_LOG_ERROR, "test", "faulting thread returned?! (filter not run)");
    return 3;
}

/* ---------------------------------------------------------------------- the driver */
static long long newest_dmp_size(void)
{
    WIN32_FIND_DATAA fd;
    long long best = -1;
    FILETIME bt = {0, 0};
    HANDLE h = FindFirstFileA("logs\\Killzone3-PC-crash-*.dmp", &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &bt) >= 0) {
            bt = fd.ftLastWriteTime;
            best = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best;
}

static DWORD run_variant(const char* exe, const char* v, long long* dmp_size)
{
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, v);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    DWORD code = 0xFFFFFFFFu;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("[test_crash_dump] %s: CreateProcess failed (%lu)\n", v, (unsigned long)GetLastError());
        return code;
    }
    if (WaitForSingleObject(pi.hProcess, 90000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99);
        printf("[test_crash_dump] %s: TIMEOUT\n", v);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    *dmp_size = newest_dmp_size();
    return code;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: test_crash_dump.exe --all | bug | fix\n");
        return 2;
    }
    if (strcmp(argv[1], "--all") != 0) return child(argv[1]);
    char exe[1024];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    static const char* const vs[] = {"bug", "fix"};
    int pass = 0;
    for (int i = 0; i < 2; i++) {
        /* a fresh logs dir per variant so "newest dump" is this variant's */
        system("if exist logs rmdir /s /q logs");
        long long sz = -1;
        DWORD code = run_variant(exe, vs[i], &sz);
        printf("[test_crash_dump] %-9s exit %ld (0x%08lX) dump %lld bytes%s\n", vs[i], (long)code, (unsigned long)code,
               sz, code == 70 ? "  (filter completed)" : "  (filter did NOT complete)");
        if (!strcmp(vs[i], "fix") && code == 70 && sz > 0) pass = 1;
    }
    printf("[test_crash_dump] %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
