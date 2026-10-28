/*
 * KZ3 port: NP sign-in and ticket calls (see np_kz3.h).
 */
#include "np_kz3.h"
#include "net_policy.h"                      /* ps3recomp libs/network (patch 4001) */
#include "ppu_context.h"                     /* ps3recomp runtime/ppu */
#include "ppu_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ps3recomp: the HLE table, the sysutil callback queue, np_psnr's identity, sceNp's callback (patch 4003). */
void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
int32_t  cellSysutilQueueGuestCallbackArgs(uint32_t opd, const uint64_t args[8]);
void     np_psnr_setup(const char* username, const char* server);
uint32_t np_manager_callback_get(uint32_t* arg);

#define SCE_NP_ERROR_INVALID_ARGUMENT 0x8002AA03u
#define SCE_NP_ERROR_INVALID_STATE    0x8002AA0Au
#define SCE_NP_ERROR_OFFLINE          0x8002AA0Cu

static char    s_online_id[17];
/* Ticket source: built in (unsigned v2.1, NP_TICKET_V21_SIZE bytes) unless the player gave a ticket file. */
static uint8_t s_ticket[NP_TICKET_FILE_MAX];
static size_t  s_ticket_len;
static int     s_have_ticket;
static uint8_t s_file_ticket[NP_TICKET_FILE_MAX];
static size_t  s_file_len;               /* 0 = built-in source */
static char    s_file_service[25];
static int     s_service_warned;

int np_kz3_set_ticket_file(const uint8_t* buf, size_t len, NpTicketInfo* info)
{
    NpTicketInfo t;
    int rc = np_ticket_check(buf, len, &t);
    if (rc) return rc;
    memcpy(s_file_ticket, buf, len);
    s_file_len = len;
    memcpy(s_file_service, t.service_id, sizeof(s_file_service));
    if (info) *info = t;
    return 0;
}

size_t np_kz3_ticket_source_file(void) { return s_file_len; }

/* A guest range [ea, ea+len) inside the 32-bit space and not at the null page. */
static int guest_range_ok(uint32_t ea, uint64_t len)
{
    return ea >= 0x10000u && (uint64_t)ea + len <= 0x100000000ull;
}

int32_t np_kz3_request_ticket2(uint32_t npid_ea, uint32_t version_ea, uint32_t service_id_ea, uint32_t cookie_ea,
                               uint32_t cookie_size, uint32_t entitlement_ea, uint32_t consumed)
{
    (void)npid_ea; (void)cookie_ea; (void)entitlement_ea; (void)consumed;
    if (!ps3_net_np_signed_in() || !s_online_id[0]) return (int32_t)SCE_NP_ERROR_OFFLINE;
    if (cookie_size > 1024) return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;

    NpTicketFields f;
    memset(&f, 0, sizeof(f));
    memcpy(f.online_id, s_online_id, sizeof(f.online_id));
    /* serviceId: a guest C string; at most 24 bytes are read, one at a time, stopping at NUL. */
    if (service_id_ea) {
        if (!guest_range_ok(service_id_ea, 1)) return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;
        for (int i = 0; i < 24 && guest_range_ok(service_id_ea + (uint32_t)i, 1); i++) {
            char c = (char)vm_read8(service_id_ea + (uint32_t)i);
            if (!c) break;
            f.service_id[i] = c;
        }
    }
    unsigned major = 0, minor = 0;
    if (version_ea && guest_range_ok(version_ea, 4)) {
        major = vm_read16(version_ea);
        minor = vm_read16(version_ea + 2);
    }
    uint64_t now = (uint64_t)time(NULL) * 1000u;
    f.issued_ms = now;
    f.expire_ms = now + 10u * 60u * 1000u;
    memcpy(f.region, "us", 2);
    memcpy(f.domain, "un", 2);
    if (s_file_len) {
        /* the player's ticket, handed over unchanged (never printed) */
        memcpy(s_ticket, s_file_ticket, s_file_len);
        s_ticket_len = s_file_len;
        if (!s_service_warned && strcmp(f.service_id, s_file_service)) {
            printf("[kz3net]   the ticket file was issued for service \"%s\", the game asks for \"%s\"\n",
                   s_file_service, f.service_id);
            s_service_warned = 1;
        }
    } else {
        if (np_ticket_build(&f, s_ticket, sizeof(s_ticket)) != NP_TICKET_V21_SIZE) return (int32_t)SCE_NP_ERROR_INVALID_STATE;
        s_ticket_len = NP_TICKET_V21_SIZE;
    }
    s_have_ticket = 1;
    printf("[kz3net] sceNpManagerRequestTicket2(v%u.%u, service \"%s\") -> ticket for \"%s\" (%u bytes, %s)\n",
           major, minor, f.service_id, s_online_id, (unsigned)s_ticket_len,
           s_file_len ? "from the ticket file" : "built in, unsigned");

    uint32_t arg = 0, cb = np_manager_callback_get(&arg);
    if (cb) {
        const uint64_t args[8] = { (uint64_t)NP_KZ3_EVENT_GOT_TICKET, (uint64_t)s_ticket_len, arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(cb, args);
    } else {
        printf("[kz3net]   no manager callback registered: GOT_TICKET not delivered\n");
    }
    return 0;
}

int32_t np_kz3_get_ticket(uint32_t buffer_ea, uint32_t size_ea)
{
    if (!size_ea || !guest_range_ok(size_ea, 4)) return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;
    if (!s_have_ticket) return (int32_t)SCE_NP_ERROR_INVALID_STATE;
    if (!buffer_ea) {                          /* size query */
        vm_write32(size_ea, (uint32_t)s_ticket_len);
        return 0;
    }
    uint32_t cap = vm_read32(size_ea);
    if (cap < s_ticket_len || !guest_range_ok(buffer_ea, s_ticket_len))
        return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_memcpy_to(buffer_ea, s_ticket, s_ticket_len);
    vm_write32(size_ea, (uint32_t)s_ticket_len);
    printf("[kz3net] sceNpManagerGetTicket -> %u bytes\n", (unsigned)s_ticket_len);
    return 0;
}

/* ---- Test-only login trace (KZ3_NET_TRACE=1, network pass 5) ----------------------------------------------------------
 * A host thread that watches the game's NP manager object and the login state machine and prints every change, so a
 * run shows which step fails and with which code. v1.00 addresses: the NP manager singleton pointer *0x1340A64 (the
 * manager callback thunk 0x16E72C loads it), its state +0xA8 (0xF = failed), error +0xAC, sub-object +0xDC (printed with
 * its vtable), the flag +0xC0; the login state machine *0x1340720, state +8. Read-only; started from the first login HLE
 * call (the EULA check), so guest memory is mapped. */
#define NP_TRACE_MGR_PTR   0x01340A64u
#define NP_TRACE_LOGIN_PTR 0x01340720u
static int s_trace_started;

static int trace_addr_ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u && !(ea & 3); }

static void np_trace_poll(void)
{
    static uint32_t last[8];
    static int have;
    uint32_t v[8] = {0};
    uint32_t mgr = vm_read32(NP_TRACE_MGR_PTR), login = vm_read32(NP_TRACE_LOGIN_PTR);
    v[0] = mgr; v[1] = login;
    if (trace_addr_ok(mgr)) {
        v[2] = vm_read32(mgr + 0xA8); v[3] = vm_read32(mgr + 0xAC); v[4] = vm_read32(mgr + 0xDC);
        v[5] = vm_read8(mgr + 0xC0);
        if (trace_addr_ok(v[4])) v[6] = vm_read32(v[4]);
    }
    if (trace_addr_ok(login)) v[7] = vm_read32(login + 8);
    if (have && !memcmp(v, last, sizeof(v))) return;
    memcpy(last, v, sizeof(v)); have = 1;
    printf("[kz3net-trace] %lu ms: mgr=0x%08X state=%u err=0x%08X sub=0x%08X(vt 0x%08X) c0=%u | login=0x%08X state=%u\n",
           (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC), v[0], v[2], v[3], v[4], v[6], v[5], v[1], v[7]);
    fflush(stdout);
}

#ifdef _WIN32
#  include <windows.h>
static DWORD WINAPI np_trace_thread(LPVOID u) { (void)u; for (;;) { np_trace_poll(); Sleep(2); } return 0; }
#else
#  include <pthread.h>
#  include <unistd.h>
static void* np_trace_thread(void* u) { (void)u; for (;;) { np_trace_poll(); usleep(2000); } return NULL; }
#endif

static void np_trace_start(void)
{
    if (s_trace_started) return;
    const char* e = getenv("KZ3_NET_TRACE");
    if (!e || !*e || !strcmp(e, "0")) return;
    s_trace_started = 1;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, np_trace_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, np_trace_thread, NULL) == 0) pthread_detach(t);
#endif
    printf("[kz3net-trace] login trace on (KZ3_NET_TRACE)\n");
}

#define R32(n)   ((uint32_t)ctx->gpr[n])
#define RET_S(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))

static void h_request_ticket2(ppu_context* ctx)
{
    RET_S(np_kz3_request_ticket2(R32(3), R32(4), R32(5), R32(6), R32(7), R32(8), R32(9)));
}

static void h_get_ticket(ppu_context* ctx) { RET_S(np_kz3_get_ticket(R32(3), R32(4))); }

/* NpEula (sysutil): KZ3's login checks the EULA state before it connects (state machine 0x16FE5C..0x170394: GetAccountRegion,
 * LoadModuleEx, then CheckEulaStatus at 0x17022C with callback OPD 0x12DE728 -> 0x170614, which takes status 1/2 as
 * accepted, 3 as rejected, an error < 0 as a failure). Without a handler the call returns 0 and the callback never
 * comes: the menu stays on "Connecting.." (network pass 4, run net-r4-capture4). The NIDs are unnamed in the import table; the
 * roles follow from the arguments (5 = check, 4 = show, 0 = abort). ASSUMPTION: status 2 = ALREADY_ACCEPTED, 1 =
 * ACCEPTED (the order of the SDK's dialog status enum; the callback treats both alike). */
static void h_eula_check(ppu_context* ctx)    /* (commId, u32, u64, cb, cbArg) */
{
    uint32_t cb = R32(6), arg = R32(7);
    np_trace_start();
    if (!cb || !guest_range_ok(cb, 8)) { RET_S(SCE_NP_ERROR_INVALID_ARGUMENT); return; }
    const uint64_t args[8] = { NP_KZ3_EULA_ALREADY_ACCEPTED, 0, arg, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(cb, args);
    printf("[kz3net] NpEula check -> already accepted\n");
    RET_S(0);
}

static void h_eula_show(ppu_context* ctx)     /* (commId, u64, cb, cbArg) */
{
    uint32_t cb = R32(5), arg = R32(6);
    if (!cb || !guest_range_ok(cb, 8)) { RET_S(SCE_NP_ERROR_INVALID_ARGUMENT); return; }
    const uint64_t args[8] = { NP_KZ3_EULA_ACCEPTED, 0, arg, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(cb, args);
    printf("[kz3net] NpEula show -> accepted\n");
    RET_S(0);
}

static void h_eula_abort(ppu_context* ctx) { RET_S(0); }

/* sceNpManagerGetAccountRegion(SceNpCountryCode* cc {char data[2], term, pad}, s32* language): the disc's region (US). */
int32_t np_kz3_get_account_region(uint32_t cc_ea, uint32_t lang_ea)
{
    if (!ps3_net_np_signed_in()) return (int32_t)SCE_NP_ERROR_OFFLINE;
    if (!cc_ea || !guest_range_ok(cc_ea, 4) || (lang_ea && !guest_range_ok(lang_ea, 4)))
        return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_write8(cc_ea, 'u'); vm_write8(cc_ea + 1, 's'); vm_write8(cc_ea + 2, 0); vm_write8(cc_ea + 3, 0);
    if (lang_ea) vm_write32(lang_ea, 1);       /* CELL_SYSUTIL_LANG_ENGLISH_US */
    return 0;
}

static void h_get_account_region(ppu_context* ctx) { RET_S(np_kz3_get_account_region(R32(3), R32(4))); }

void np_kz3_sign_in(const char* online_id)
{
    if (!ps3_net_online() || !np_ticket_valid_online_id(online_id)) return;
    snprintf(s_online_id, sizeof(s_online_id), "%s", online_id);
    np_psnr_setup(s_online_id, NULL);          /* sceNpGetNpId / GetOnlineId answer with it */
    ps3_net_set_np_signed_in(1);               /* sceNpManagerGetStatus + RegisterCallback: ONLINE (patch 4003) */
    ps3_hle_register_ctx(NP_KZ3_NID_REQUEST_TICKET2, "sceNpManagerRequestTicket2", h_request_ticket2);
    ps3_hle_register_ctx(NP_KZ3_NID_GET_TICKET, "sceNpManagerGetTicket", h_get_ticket);
    ps3_hle_register_ctx(NP_KZ3_NID_GET_ACCOUNT_REGION, "sceNpManagerGetAccountRegion", h_get_account_region);
    ps3_hle_register_ctx(NP_KZ3_NID_EULA_CHECK, "cellSysutilNpEula check", h_eula_check);
    ps3_hle_register_ctx(NP_KZ3_NID_EULA_SHOW, "cellSysutilNpEula show", h_eula_show);
    ps3_hle_register_ctx(NP_KZ3_NID_EULA_ABORT, "cellSysutilNpEula abort", h_eula_abort);
    printf("[kz3net] NP signed in as \"%s\" (ticket calls registered)\n", s_online_id);
}
