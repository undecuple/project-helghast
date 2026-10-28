/* kz3_log: the one logger of the Killzone 3 port and its runtime.
 *
 * Portable C11 (MinGW-w64 and Linux). Every emitted line carries the time in ms since kz3_log_init, the OS thread
 * id, a level letter and a tag:
 *
 *     log file:  "   123456  4321 I [fs] open '/dev_bdvd/...'"
 *     stdout:    "[fs] open '/dev_bdvd/...'"          (test builds only: today's format, kz3_metrics.py parses it)
 *
 * Outputs: a file <dir>/logs/Killzone3-PC-YYYYMMDD-HHMMSS.log (newest 10 kept), optionally stdout (KZ3_LOG_F_STDOUT or
 * KZ3_LOG_STDOUT=1), and an in-memory ring of the last 200 lines that goes into crash reports.
 *
 * Filtering: each tag has a threshold (KZ3_LOG_LEVEL sets the default, info if unset; KZ3_LOG="tag:level,..."
 * overrides per tag, "*:level" is the default too). A line is emitted when its level <= the tag's threshold.
 * Levels: error, warn, info, debug, trace (also e/w/i/d/t or 0..4; "off" silences a tag).
 *
 * Hot path: no global lock. The enabled check is one atomic load plus (only when per-tag overrides exist) a short
 * scan of the override table; an emitted line is formatted on the caller's stack, copied into a lock-free ring
 * slot and written with one OS write call (file opened for append; nothing is buffered, so errors are on disk at
 * once and nothing is lost when the process is killed).
 *
 * The line router (kz3_log_line / kz3_log_printf / the KZ3_LOG_REDIRECT_STDIO macros below) lets the runtime's
 * existing "[tag] ..." printf diagnostics go through the logger unchanged: the leading "[tag]" (plus, for a few
 * tags, the first word after it) is looked up in a built-in table that gives the message level; unknown tags are
 * info. For printf-style calls the tag is read from the format string, so a filtered-out line is never formatted.
 */
#ifndef KZ3_LOG_H
#define KZ3_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define KZ3_LOG_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define KZ3_LOG_PRINTF(f, a)
#endif

enum {
    KZ3_LOG_OFF   = -1,
    KZ3_LOG_ERROR = 0,
    KZ3_LOG_WARN  = 1,
    KZ3_LOG_INFO  = 2,
    KZ3_LOG_DEBUG = 3,
    KZ3_LOG_TRACE = 4
};

enum {
    KZ3_LOG_F_STDOUT = 1u,   /* also echo emitted lines to stdout in today's "[tag] text" format (test builds) */
    KZ3_LOG_F_NO_ENV = 2u,   /* ignore KZ3_LOG / KZ3_LOG_LEVEL / KZ3_LOG_STDOUT (tests) */
    KZ3_LOG_F_STDERR_ERRORS = 4u /* also echo error lines to stderr (when not echoing everything to stdout) */
};

#define KZ3_LOG_KEEP_FILES   10   /* newest Killzone3-PC-*.log and Killzone3-PC-crash-*.txt kept in <dir>/logs */
#define KZ3_LOG_NAME_PREFIX  "Killzone3-PC-"   /* every file name in <dir>/logs (ui-review row 27, pass 38) */
#define KZ3_LOG_RING_LINES   200
#define KZ3_LOG_LINE_MAX     1024 /* longer messages are truncated ("..." at the end) */
#define KZ3_LOG_FATAL_EXIT   70   /* exit code when a fatal error stops the process */

/* Start the logger. dir: directory that gets the "logs" subdirectory (the exe's directory); NULL = no log file
 * (ring and stdout only). Safe to call once; a second call returns -1. Returns 0, or 1 when the log file could not
 * be created (logging then goes to the ring and stdout only). Lines logged before init go to the ring only. */
int  kz3_log_init(const char* dir, unsigned flags);
void kz3_log_shutdown(void);                 /* closes the file; later lines go to the ring (and stdout) only */
const char* kz3_log_file_path(void);         /* "" when there is no log file */
const char* kz3_log_dir(void);               /* "<dir>/logs" or "" */

/* Filtering. kz3_log_configure parses a KZ3_LOG-style spec ("mfc-list:trace,spurs-pm:debug,*:warn") and replaces
 * the per-tag table (rare: allocates a new table; the old one is kept alive for racing readers). */
int  kz3_log_configure(const char* spec);
void kz3_log_set_default_level(int level);
int  kz3_log_threshold(const char* tag, size_t tag_len);   /* the tag's threshold */
int  kz3_log_enabled(int level, const char* tag);
int  kz3_log_parse_level(const char* s, size_t n);         /* -2 if not a level name */

/* Logging. tag: short subsystem name without brackets ("fs", "hle", "spurs"). */
void kz3_log(int level, const char* tag, const char* fmt, ...) KZ3_LOG_PRINTF(3, 4);
void kz3_logv(int level, const char* tag, const char* fmt, va_list ap);
/* Label printed after the thread id for the calling thread (e.g. "ppu:7", "spu:2"); NULL clears. Copied. */
void kz3_log_set_thread_label(const char* label);

/* Once-only helpers. kz3_log_once_flag returns 1 the first time for *flag (atomic). kz3_log_once_key returns 1 the
 * first time a (tag, key) pair is seen (lock-free set of 4096 entries; when full it returns 0: stay quiet). */
int  kz3_log_once_flag(int* flag);
int  kz3_log_once_key(const char* tag, uint64_t key);

#define KZ3_LOG_AT(lvl, tag, ...) \
    do { if (kz3_log_enabled((lvl), (tag))) kz3_log((lvl), (tag), __VA_ARGS__); } while (0)
#define KZ3_LOGE(tag, ...) KZ3_LOG_AT(KZ3_LOG_ERROR, tag, __VA_ARGS__)
#define KZ3_LOGW(tag, ...) KZ3_LOG_AT(KZ3_LOG_WARN,  tag, __VA_ARGS__)
#define KZ3_LOGI(tag, ...) KZ3_LOG_AT(KZ3_LOG_INFO,  tag, __VA_ARGS__)
#define KZ3_LOGD(tag, ...) KZ3_LOG_AT(KZ3_LOG_DEBUG, tag, __VA_ARGS__)
#define KZ3_LOGT(tag, ...) KZ3_LOG_AT(KZ3_LOG_TRACE, tag, __VA_ARGS__)
/* Once per call site. */
#define KZ3_LOG_ONCE(lvl, tag, ...) \
    do { static int kz3_once_; if (kz3_log_once_flag(&kz3_once_)) kz3_log((lvl), (tag), __VA_ARGS__); } while (0)
/* Once per (tag, key): e.g. key = NID for unresolved imports, key = guest lr for null-page stores. */
#define KZ3_LOG_ONCE_KEY(lvl, tag, key, ...) \
    do { if (kz3_log_enabled((lvl), (tag)) && kz3_log_once_key((tag), (uint64_t)(key))) \
             kz3_log((lvl), (tag), __VA_ARGS__); } while (0)

/* Line router. kz3_log_route maps a finished line's leading "[tag]" (and first word) to a message level and
 * copies the tag (without brackets) to tag_out; returns KZ3_LOG_INFO and tag "" for an untagged line.
 * kz3_log_line logs one or more '\n'-separated lines (untagged continuation lines -- starting with a blank, a hex
 * digit or punctuation -- take the level and tag of the calling thread's previous routed line).
 * kz3_log_printf / kz3_log_vprintf accept printf calls, keep partial lines per thread until '\n', and skip
 * formatting entirely when the format string's own "[tag]" is filtered out. */
int  kz3_log_route(const char* line, size_t len, char* tag_out, size_t tag_cap);
void kz3_log_line(const char* text);
int  kz3_log_printf(const char* fmt, ...) KZ3_LOG_PRINTF(1, 2);
int  kz3_log_vprintf(const char* fmt, va_list ap);
int  kz3_log_fprintf(FILE* f, const char* fmt, ...) KZ3_LOG_PRINTF(2, 3);
int  kz3_log_vfprintf(FILE* f, const char* fmt, va_list ap);
int  kz3_log_puts(const char* s);
int  kz3_log_fputs(const char* s, FILE* f);
int  kz3_log_fflush(FILE* f);
void kz3_log_flush(void);                    /* emits a pending partial line of the calling thread */

/* Fatal errors. kz3_log_fatal logs an error line, writes <dir>/logs/Killzone3-PC-crash-YYYYMMDD-HHMMSS.txt (the message and the
 * ring) and then:
 *   - KZ3_CONTINUE_ON_FATAL=1: returns (today's behaviour, for investigations; the hook is not called);
 *   - else calls the hook (the port's error box). Hook returns nonzero: kz3_log_fatal returns. Hook returns 0, or
 *     no hook is set: the process exits with KZ3_LOG_FATAL_EXIT.
 * A second fatal on another thread while the first is being reported waits for it; a fatal raised from inside
 * the hook exits at once. */
typedef int (*kz3_log_fatal_hook)(const char* message, const char* crash_file, void* user);
void kz3_log_set_fatal_hook(kz3_log_fatal_hook fn, void* user);
void kz3_log_fatal(const char* tag, const char* fmt, ...) KZ3_LOG_PRINTF(2, 3);
/* For exception filters and signal paths: write a crash file (reason, details, ring) without calling the hook and
 * without allocating. Returns the crash file path ("" if it could not be written). */
const char* kz3_log_write_crash(const char* reason, const char* details);
/* Windows: writes a MiniDumpNormal minidump beside crash_path (".txt" -> ".dmp") for the faulting thread
 * faulting_tid with its EXCEPTION_POINTERS, from a helper thread (Microsoft's guidance: not on the faulting thread,
 * whose stack may be gone), through dbghelp loaded at kz3_log_init (no LoadLibrary at crash time). Uses the SDK's
 * 4-byte-packed MINIDUMP_EXCEPTION_INFORMATION: a naturally aligned hand-rolled struct made dbghelp read the pointer
 * at the wrong offset and fault (RSX pass 33). Logs the outcome as [CRASH]; returns 1 when the dump was written.
 * Other platforms: returns 0. */
int kz3_log_write_minidump(const char* crash_path, void* exception_pointers, unsigned long faulting_tid);

/* Ring snapshot: oldest to newest, each line '\n'-terminated, as written to the log file (with prefix). Returns the
 * number of bytes written to buf (always NUL-terminated when cap > 0). */
size_t kz3_log_ring_snapshot(char* buf, size_t cap);

/* Game hooks (RE pass 64, the RE notes section 34). The bodies live here, keyed by the guest address
 * of the function the override replaces, so one copy serves v1.00 and 1.14; the overrides themselves are one macro
 * line each (below) in port/spu/kz3_ppu_overrides.cpp and one address line in port/spu/ppu_overrides.txt.
 *
 *   function                        v1.00       1.14        shape
 *   HardExit(fmt, ...)              0x0008AB70  0x0008C360  vsnprintf(r1+0x70, 0x1000, fmt, va), [1.14: strlen +
 *                                                           SetCrashDumpField(3, buf, len)], stw 0x0DEADCA7 -> [0]
 *   SetCrashDumpField(id, p, len)   (none)      0x0008CF08  id 0..13 -> key (3 HardExitMessage, 10 StreamingLump)
 *   ResolveDependentLink(ctx,?,key,holder)
 *                                   0x000DDFF0  0x000DFF30  returns 0 (linked) or 8 ("Unresolved Dependent Link")
 */
#define KZ3_ADDR_HARDEXIT_100         0x0008AB70u
#define KZ3_ADDR_HARDEXIT_114         0x0008C360u
#define KZ3_ADDR_CRASHDUMP_FIELD_114  0x0008CF08u
#define KZ3_ADDR_LINK_RESOLVE_100     0x000DDFF0u
#define KZ3_ADDR_LINK_RESOLVE_114     0x000DFF30u
#define KZ3_LINK_MISS_MAX             64      /* [link-miss] lines per run (one per distinct target), then one note */
#define KZ3_HOOK_TEXT_MAX             300     /* bytes of guest text copied by the hooks */

/* HardExit: call AFTER the lifted body returned (the message is then formatted in the dead frame below sp).
 * fn = the HardExit address (KZ3_ADDR_HARDEXIT_*), sp = r1 and lr = ctx->lr on entry, vm = guest memory base.
 * Logs "[HardExit] <text> (caller lr=..., @fn, game v)" once per (lr, text) at error level, or with
 * KZ3_HARDEXIT=fatal through kz3_log_fatal (crash file + the error box). Returns 1 when a line was logged, 0 for
 * an unknown fn, no vm, an empty message or a repeat. */
int  kz3_log_hardexit(uint32_t fn, uint32_t sp, uint32_t lr, const uint8_t* vm);
/* SetCrashDumpField(id, text, len) (1.14 only; call before or after the body): keeps field 10 (StreamingLump) for
 * [link-miss]; logs the other fields at debug under [crashdump] ("Level='...'"); field 3 is left to kz3_log_hardexit.
 * text = vm + r4 (host pointer, already range-checked by the caller), len = r5. */
void kz3_log_crashdump_field(uint32_t id, const uint8_t* text, uint32_t len);
const char* kz3_log_streaming_lump(void);     /* last field 10 text, "" before the first */
/* [link-miss]: call AFTER the lifted link-resolve body with status = r3, key = a copy of the 16-byte target hash r5
 * pointed at on entry (in-memory word order; the .core text has each word byte-reversed, pass 58), holder = r6,
 * lr = ctx->lr on entry. Nothing happens unless status == 8. Once per target, KZ3_LINK_MISS_MAX lines per run.
 * Returns 1 when a line was logged. */
int  kz3_log_link_miss(uint32_t status, const uint8_t key[16], uint32_t holder, uint32_t lr);

/* Override glue for port/spu/kz3_ppu_overrides.cpp (needs ppu_context, vm_base and the func_<ADDR>__lifted body):
 *   KZ3_HOOK_HARDEXIT(0008AB70)       v1.00     KZ3_HOOK_HARDEXIT(0008C360)       1.14
 *   KZ3_HOOK_LINK_RESOLVE(000DDFF0)   v1.00     KZ3_HOOK_LINK_RESOLVE(000DFF30)   1.14
 *   KZ3_HOOK_CRASHDUMP_FIELD(0008CF08)          1.14 only
 * The link hook copies 16 bytes before the body and compares r3 after it: that is its whole cost when nothing
 * misses. KZ3_GUEST_PTR_OK(p) is the range check the other overrides use. */
#define KZ3_GUEST_PTR_OK(p) ((uint32_t)(p) >= 0x10000u && (uint32_t)(p) < 0xF0000000u)
#define KZ3_HOOK_HARDEXIT(ADDR) \
    void func_##ADDR(ppu_context* ctx) { \
        uint32_t sp_ = (uint32_t)ctx->gpr[1], lr_ = (uint32_t)ctx->lr; \
        func_##ADDR##__lifted(ctx); \
        kz3_log_hardexit((uint32_t)0x##ADDR, sp_, lr_, vm_base); }
#define KZ3_HOOK_LINK_RESOLVE(ADDR) \
    void func_##ADDR(ppu_context* ctx) { \
        uint32_t keyp_ = (uint32_t)ctx->gpr[5], holder_ = (uint32_t)ctx->gpr[6], lr_ = (uint32_t)ctx->lr; \
        uint8_t key_[16]; int ok_ = vm_base && KZ3_GUEST_PTR_OK(keyp_); \
        if (ok_) memcpy(key_, vm_base + keyp_, 16); \
        func_##ADDR##__lifted(ctx); \
        if (ok_ && (uint32_t)ctx->gpr[3] == 8u) kz3_log_link_miss(8u, key_, holder_, lr_); }
#define KZ3_HOOK_CRASHDUMP_FIELD(ADDR) \
    void func_##ADDR(ppu_context* ctx) { \
        uint32_t p_ = (uint32_t)ctx->gpr[4]; \
        if (vm_base && KZ3_GUEST_PTR_OK(p_)) \
            kz3_log_crashdump_field((uint32_t)ctx->gpr[3], vm_base + p_, (uint32_t)ctx->gpr[5]); \
        func_##ADDR##__lifted(ctx); }

#ifdef __cplusplus
}
#endif

/* Stdio redirect for the runtime: compile ps3recomp sources with -DKZ3_LOG_REDIRECT_STDIO -include kz3_log.h and
 * their printf / fprintf(stderr|stdout) / puts / fputs / fflush(stderr|stdout) calls go through the router.
 * fprintf to any other FILE* is passed through unchanged. <stdio.h> (and <cstdio>, whose libstdc++ version
 * #undefs printf) are included first so later includes cannot undo the macros. */
#if defined(KZ3_LOG_REDIRECT_STDIO) && !defined(KZ3_LOG_IMPLEMENTATION)
#  ifdef __cplusplus
#    include <cstdio>
#  endif
#  undef printf
#  undef fprintf
#  undef vprintf
#  undef vfprintf
#  undef puts
#  undef fputs
#  undef fflush
#  define printf(...)            kz3_log_printf(__VA_ARGS__)
#  define fprintf(f, ...)        kz3_log_fprintf((f), __VA_ARGS__)
#  define vprintf(fmt, ap)       kz3_log_vprintf((fmt), (ap))
#  define vfprintf(f, fmt, ap)   kz3_log_vfprintf((f), (fmt), (ap))
#  define puts(s)                kz3_log_puts(s)
#  define fputs(s, f)            kz3_log_fputs((s), (f))
#  define fflush(f)              kz3_log_fflush(f)
#endif

#endif /* KZ3_LOG_H */
