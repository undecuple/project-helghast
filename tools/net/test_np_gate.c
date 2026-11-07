/* Exercise KZ3 registration with the patched NP manager, Score, Matching2 and psnr gate. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET test_socket;
#  define TEST_CALL WSAAPI
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
typedef int test_socket;
#  define TEST_CALL
#endif

#include "np_psnr.h"
#include "sceNp.h"
#include "sceNpMatching2.h"
#include "ppu_memory.h"

extern uint32_t ppu_vm_size;

static int resolver_calls, socket_calls, transport_calls, registrations, callbacks;
static uint64_t callback_args[8];
static struct sockaddr_in fake_address;
static struct addrinfo fake_result;

static int TEST_CALL fake_getaddrinfo(const char* host, const char* service,
                                     const struct addrinfo* hints, struct addrinfo** result)
{
    (void)host; (void)service; (void)hints;
    resolver_calls++;
    if (getenv("KZ3_NET_TEST_RESOLVE_FAIL")) return EAI_NONAME;
    memset(&fake_address, 0, sizeof(fake_address));
    fake_address.sin_family = AF_INET;
    const uint8_t ip[4] = {192, 0, 2, 47};
    memcpy(&fake_address.sin_addr, ip, sizeof(ip));
    memset(&fake_result, 0, sizeof(fake_result));
    fake_result.ai_family = AF_INET;
    fake_result.ai_addr = (struct sockaddr*)&fake_address;
    fake_result.ai_addrlen = sizeof(fake_address);
    *result = &fake_result;
    return 0;
}

static void TEST_CALL fake_freeaddrinfo(struct addrinfo* result) { assert(result == &fake_result); }

#ifdef _WIN32
static SOCKET TEST_CALL fake_socket(int family, int type, int protocol)
#else
static int fake_socket(int family, int type, int protocol)
#endif
{
    (void)family; (void)type; (void)protocol;
    socket_calls++;
#ifdef _WIN32
    return INVALID_SOCKET;
#else
    return -1;
#endif
}

#ifdef _WIN32
static int TEST_CALL fake_send(test_socket s, const char* data, int len, int flags)
#else
static ssize_t fake_send(test_socket s, const void* data, size_t len, int flags)
#endif
{
    (void)s; (void)data; (void)len; (void)flags;
    assert(!"unexpected capture send");
    return -1;
}

#ifdef _WIN32
static int TEST_CALL fake_wsa_startup(WORD version, LPWSADATA data)
{
    (void)version;
    memset(data, 0, sizeof(*data));
    return 0;
}
#  define WSAStartup fake_wsa_startup
#endif
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define socket fake_socket
#define send fake_send
#include "../../port/net/kz3_net.c"
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef send
#ifdef _WIN32
#  undef WSAStartup
#endif

psnr_client* fixture_psnr_connect(const char* host, uint16_t port, const char* comm_id,
                                  const char* online_id, uint16_t p2p_port,
                                  uint32_t* user_id, uint8_t public_ip[4])
{
    assert(!strcmp(host, "psnr-fixture.invalid") && port == 36100);
    assert(comm_id && online_id && user_id && public_ip);
    (void)p2p_port;
    transport_calls++;
    return NULL;   /* Even the positive controls never open a host socket. */
}

void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    assert(nid && name && fn);
    registrations++;
}

int32_t cellSysutilQueueGuestCallbackArgs(uint32_t opd, const uint64_t args[8])
{
    assert(opd == 0x18000u);
    callbacks++;
    memcpy(callback_args, args, sizeof(callback_args));
    return 0;
}

void edat_add_klicensee(const uint8_t key[16]) { (void)key; }

extern int32_t sceNpScoreCreateTitleCtx(uint32_t comm_id, uint32_t passphrase, uint32_t self_np_id);
extern int32_t sceNpScoreDestroyTitleCtx(int32_t title);
extern int kz3_hle_register(void);

enum { STATUS = 0x10000, COMM = 0x10100, CTX = 0x10200 };

static int manager_status(void)
{
    assert(sceNpManagerGetStatus((int32_t*)(uintptr_t)STATUS) == 0);
    return (int32_t)vm_read32(STATUS);
}

static void try_score(void)
{
    assert(sceNpScoreInit() == 0);
    int32_t title = sceNpScoreCreateTitleCtx(COMM, 0, 0);
    assert(title > 0);
    assert(sceNpScoreDestroyTitleCtx(title) == 0);
    assert(sceNpScoreTerm() == 0);
}

static void try_matching(void)
{
    assert(sceNpMatching2Init2(0, 0, 0) == 0);
    assert(sceNpMatching2CreateContext(0, COMM, 0, CTX, 0) == 0);
    uint16_t ctx = vm_read16(CTX);
    assert(ctx && sceNpMatching2RegisterContextCallback(ctx, 0x18000u, 0x1234u) == 0);
    assert(sceNpMatching2ContextStartAsync(ctx, 0) == 0);
    assert(callbacks == 1 && callback_args[0] == ctx && callback_args[1] == 0x6F01);
    assert(callback_args[3] == SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE);
    assert(sceNpMatching2DestroyContext(ctx) == 0);
    assert(sceNpMatching2Term() == 0);
}

static void test_valid_registration(void)
{
    const uint8_t ip[4] = {192, 0, 2, 47};
    uint8_t mapped[4];
    assert(!getenv("PSNR_SERVER") && ps3_net_online() && !np_psnr_enabled());
    assert(kz3_hle_register() == 0);
    assert(registrations == 10 && resolver_calls == 1 && !socket_calls);
    assert(np_kz3_ticket_source_file() == NP_TICKET_V21_SIZE && ps3_net_np_signed_in());
    assert(ps3_net_policy_resolve(KZ3_NET_MUIS_HOST, mapped) == 1 && !memcmp(mapped, ip, 4));
    assert(ps3_net_policy_check(PS3_NET_PROTO_TCP, ip, 10071));
    assert(sceNpInit(131072, (void*)(uintptr_t)0x8000u) == 0);
    assert(manager_status() == SCE_NP_MANAGER_STATUS_ONLINE);
    assert(sceNpManagerRegisterCallback((SceNpManagerCallback)(uintptr_t)0x18000u,
                                        (void*)(uintptr_t)0x1234u) == 0);
    assert(callbacks == 1 && callback_args[0] == SCE_NP_MANAGER_STATUS_ONLINE);
    assert(np_psnr_connect("NPWR00000_00") == -1 && !transport_calls);
    assert(sceNpTerm() == 0);
    puts("PASS valid HLE registration and synthetic ticket sign-in");
}

int main(int argc, char** argv)
{
    static uint8_t memory[1u << 20];
    assert(argc == 2);
    vm_base = memory;
    ppu_vm_size = sizeof(memory);
    memcpy(vm_base + COMM, "NPWR00000", 9);
    if (!strcmp(argv[1], "good")) {
        test_valid_registration();
        return 0;
    }
    assert(getenv("PSNR_SERVER") && !strcmp(getenv("PSNR_SERVER"), "psnr-fixture.invalid:36100"));
    int offline = !strcmp(argv[1], "offline");
    const char* online = offline ? "0" : "1";
    assert(getenv("KZ3_ONLINE") && !strcmp(getenv("KZ3_ONLINE"), online));
    assert(ps3_net_online() == !offline && !!np_psnr_enabled() == !offline);
    int positive = !strcmp(argv[1], "positive-score") || !strcmp(argv[1], "positive-matching");
    int resolved_failure = !strcmp(argv[1], "closed-ticket") || !strcmp(argv[1], "closed-unresolved");
    if (!positive) {
        if (!offline) {
            const uint8_t stale_ip[4] = {192, 0, 2, 47};
            assert(!ps3_net_policy_map_host(KZ3_NET_MUIS_HOST, stale_ip));
            assert(!ps3_net_policy_allow(stale_ip, 10071, 10071, PS3_NET_PROTO_TCP));
            ps3_net_set_np_signed_in(1);
        }
        assert(kz3_hle_register() == (offline ? 0 : -1));
    }
    assert(sceNpInit(131072, (void*)(uintptr_t)0x8000u) == 0);
    int status = manager_status();
    if (!positive) {
        assert(sceNpManagerRegisterCallback((SceNpManagerCallback)(uintptr_t)0x18000u,
                                            (void*)(uintptr_t)0x1234u) == 0);
        assert(!callbacks);
        assert((uint32_t)sceNpManagerRequestTicket2(0x10300u, 0, 0x10400u, 0, 0, 0, 0)
               == SCE_NP_ERROR_OFFLINE);
    }
    if (!strcmp(argv[1], "positive-score")) try_score();
    else if (!strcmp(argv[1], "positive-matching")) try_matching();
    else {
        assert(offline || !strncmp(argv[1], "closed", 6));
        try_score();
        try_matching();
    }

    assert(resolver_calls == resolved_failure && !socket_calls);
    assert(registrations == (positive ? 0 : 4));
    assert(!strcmp(getenv("PSNR_SERVER"), "psnr-fixture.invalid:36100"));
    assert(!strcmp(getenv("KZ3_ONLINE"), online));
    if (positive) {
        assert(status == SCE_NP_MANAGER_STATUS_ONLINE && transport_calls == 1);
    } else {
        assert(np_psnr_connect("NPWR00000_00") == -1);
        assert(!transport_calls && !np_psnr_connected());
        assert(status == SCE_NP_MANAGER_STATUS_OFFLINE);
        assert(!ps3_net_online() && !np_psnr_enabled() && !ps3_net_np_signed_in());
        if (!offline) {
            const uint8_t stale_ip[4] = {192, 0, 2, 47};
            uint8_t mapped[4];
            assert(ps3_net_policy_strict());
            assert(!ps3_net_policy_resolve(KZ3_NET_MUIS_HOST, mapped));
            assert(!ps3_net_policy_check(PS3_NET_PROTO_TCP, stale_ip, 10071));
            assert(!np_kz3_ticket_source_file());
        }
    }
    assert(sceNpTerm() == 0);
    puts("PASS patched NP status and Score/Matching psnr connection gate");
    return 0;
}
