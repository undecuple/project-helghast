/* Include the port implementation to exercise its file-local RT parser and logger. */
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

#include "net_policy.h"

static int resolver_calls, file_opens, ticket_opens, socket_calls, sends;
static int policy_map_calls, policy_allow_calls;
static uint8_t sent_ids[8];
static char resolved_name[256];
static struct sockaddr_in fake_address;
static struct addrinfo fake_result;
static FILE* config_stream;

static int TEST_CALL fake_getaddrinfo(const char* host, const char* service,
                                      const struct addrinfo* hints, struct addrinfo** result)
{
    (void)service; (void)hints;
    resolver_calls++;
    snprintf(resolved_name, sizeof(resolved_name), "%s", host);
    if (getenv("KZ3_NET_TEST_RESOLVE_FAIL")) return EAI_NONAME;
    memset(&fake_address, 0, sizeof(fake_address));
    fake_address.sin_family = AF_INET;
    const uint8_t public_fixture[4] = {192, 0, 2, 47};
    const uint8_t loopback_fixture[4] = {127, 0, 0, 1};
    memcpy(&fake_address.sin_addr, getenv("KZ3_NET_TEST_LOOPBACK") ? loopback_fixture : public_fixture, 4);
    memset(&fake_result, 0, sizeof(fake_result));
    fake_result.ai_family = AF_INET;
    fake_result.ai_addr = (struct sockaddr*)&fake_address;
    fake_result.ai_addrlen = sizeof(fake_address);
    *result = &fake_result;
    return 0;
}

static void TEST_CALL fake_freeaddrinfo(struct addrinfo* result) { assert(result == &fake_result); }

#ifdef _WIN32
static int TEST_CALL fake_send(test_socket s, const char* data, int len, int flags)
#else
static ssize_t fake_send(test_socket s, const void* data, size_t len, int flags)
#endif
{
    (void)s; (void)flags;
    assert(data && len >= 3 && sends < (int)sizeof(sent_ids));
    sent_ids[sends++] = ((const uint8_t*)data)[0];
    return len;
}

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
static int TEST_CALL fake_wsa_startup(WORD version, LPWSADATA data)
{
    (void)version;
    memset(data, 0, sizeof(*data));
    return 0;
}
static FILE* fake_wfopen(const wchar_t* path, const wchar_t* mode);
#endif
static FILE* fake_fopen(const char* path, const char* mode);
static size_t fake_fread(void* ptr, size_t size, size_t count, FILE* stream);

static int counted_policy_map(const char* name, const uint8_t ip[4])
{
    policy_map_calls++;
    return ps3_net_policy_map_host(name, ip);
}

static int counted_policy_allow(const uint8_t ip[4], uint16_t lo, uint16_t hi, int protos)
{
    policy_allow_calls++;
    if (getenv("KZ3_NET_TEST_ALLOW_FAIL") && policy_allow_calls == 2) return -1;
    return ps3_net_policy_allow(ip, lo, hi, protos);
}

#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define send fake_send
#define socket fake_socket
#define fopen fake_fopen
#define fread fake_fread
#define ps3_net_policy_map_host counted_policy_map
#define ps3_net_policy_allow counted_policy_allow
#ifdef _WIN32
#  define WSAStartup fake_wsa_startup
#  define _wfopen fake_wfopen
#endif
#include "../../port/net/kz3_net.c"
#undef getaddrinfo
#undef freeaddrinfo
#undef send
#undef socket
#undef fopen
#undef fread
#undef ps3_net_policy_map_host
#undef ps3_net_policy_allow
#ifdef _WIN32
#  undef WSAStartup
#  undef _wfopen
#endif

#include "ppu_memory.h"

extern uint32_t ppu_vm_size;

static FILE* fake_fopen(const char* path, const char* mode)
{
    const char* config = getenv("KZ3_NET_CONFIG");
    const char* ticket = getenv("KZ3_NET_TEST_TICKET");
    if (config && !strcmp(path, config)) {
        file_opens++;
        if (getenv("KZ3_NET_TEST_OPEN_ERROR")) { errno = EACCES; return NULL; }
        config_stream = fopen(path, mode);
        return config_stream;
    }
    ticket_opens++;
    if (ticket && !strcmp(path, ticket)) return fopen(path, mode);
    errno = ENOENT;
    return NULL;
}

static size_t fake_fread(void* ptr, size_t size, size_t count, FILE* stream)
{
    if (stream == config_stream && getenv("KZ3_NET_TEST_SHORT_READ") && size == 1 && count)
        return fread(ptr, size, count - 1, stream);
    return fread(ptr, size, count, stream);
}

#ifdef _WIN32
static FILE* fake_wfopen(const wchar_t* path, const wchar_t* mode)
{
    wchar_t allowed[1300];
    const char* ticket = getenv("KZ3_NET_TEST_TICKET");
    ticket_opens++;
    if (ticket && MultiByteToWideChar(CP_UTF8, 0, ticket, -1, allowed, 1300) > 0 && !wcscmp(path, allowed))
        return _wfopen(path, mode);
    errno = ENOENT;
    return NULL;
}
#endif

static int registrations, callbacks, identity_calls, callback_enabled = 1;
static uint64_t callback_args[8];
static uint32_t callback_opd;

void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    assert(nid && name && fn);
    registrations++;
}

int32_t cellSysutilQueueGuestCallbackArgs(uint32_t opd, const uint64_t args[8])
{
    callbacks++;
    callback_opd = opd;
    memcpy(callback_args, args, sizeof(callback_args));
    return 0;
}

void np_psnr_setup(const char* username, const char* server)
{
    assert(username && !strcmp(username, "TestUser1") && !server);
    identity_calls++;
}

uint32_t np_manager_callback_get(uint32_t* arg)
{
    *arg = 0x12345678u;
    return callback_enabled ? 0x18000u : 0;
}

static void test_config(void)
{
    static const char selected[] =
        "\xEF\xBB\xBFserver = second\n"
        "online_id = TestUser1\n"
        "host = fallback.invalid\n"
        "[server first]\n"
        "host = first.invalid\n"
        "[server Second]\n"
        "host = selected.invalid\n"
        "ports = 10070-10080, 50000-50199\n"
        "region = \"A & B; # Safe\"\n";
    Kz3NetConfig cfg;
    kz3_net_config_defaults(&cfg);
    assert(!cfg.host[0] && cfg.nranges == 2);
    assert(!kz3_net_parse_config(selected, sizeof(selected) - 1, &cfg, NULL));
    assert(!kz3_net_config_select(&cfg, NULL));
    assert(!strcmp(cfg.host, "selected.invalid"));
    assert(!strcmp(cfg.region, "A & B; # Safe") && cfg.nranges == 2);

    static const char default_host[] = "host = fallback.invalid\nunused = ignored\n"
                                       "[server first]\nhost = first.invalid\n";
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config(default_host, sizeof(default_host) - 1, &cfg, NULL) == 1);
    assert(!kz3_net_config_select(&cfg, NULL) && !strcmp(cfg.host, "fallback.invalid"));
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config("sever = first\n", 14, &cfg, NULL) == 1);
    static const char ticket_after[] = "host = fallback.invalid\nticket_file = synthetic.ticket\n";
    assert(!kz3_net_parse_config(ticket_after, sizeof(ticket_after) - 1, &cfg, NULL));
    assert(kz3_net_config_select(&cfg, NULL) == -1);
    kz3_net_config_defaults(&cfg);
    assert(!kz3_net_parse_config(ticket_after, sizeof(ticket_after) - 1, &cfg, NULL));
    assert(kz3_net_parse_config("unknown = value\n", 16, &cfg, NULL) == 1);
    assert(kz3_net_config_select(&cfg, NULL) == -1);
    static const char rejected_ticket[] = "host = fallback.invalid\nticket_file = \"synthetic.ticket\"\n";
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config(rejected_ticket, sizeof(rejected_ticket) - 1, &cfg, NULL) == 1);
    assert(!cfg.ticket_file[0] && cfg.ticket_requested && kz3_net_config_select(&cfg, NULL) == -1);
    kz3_net_config_defaults(&cfg);
    static const char identity_unknown[] = "host = fallback.invalid\nonline_id = TestUser1\nfuture = 1\n";
    assert(kz3_net_parse_config(identity_unknown, sizeof(identity_unknown) - 1, &cfg, NULL) == 1);
    assert(kz3_net_config_select(&cfg, NULL) == -1);
    static const char default_section[] = "[server first]\nhost = first.invalid\n";
    kz3_net_config_defaults(&cfg);
    assert(!kz3_net_parse_config(default_section, sizeof(default_section) - 1, &cfg, NULL));
    assert(!kz3_net_config_select(&cfg, NULL) && !strcmp(cfg.host, "first.invalid"));
    static const char reset_section[] = "[other]\nhost = fallback.invalid\n[server]\nserver = first\n"
                                        "[server first]\nhost = first.invalid\n";
    kz3_net_config_defaults(&cfg);
    assert(!kz3_net_parse_config(reset_section, sizeof(reset_section) - 1, &cfg, NULL));
    assert(!strcmp(cfg.host, "fallback.invalid"));
    assert(!kz3_net_config_select(&cfg, NULL) && !strcmp(cfg.host, "first.invalid"));

    const char* invalid[] = {
        "server = absent\n", "server =\n", "server = bad!\n", "server = \"first\"\n",
        "server = \"unterminated\n", "server first\n", "server_name = first\n",
        "selected_server = first\n", "selectedServer = first\n", "selectedserver = first\n", "\"server\" = first\n",
        "server = first\nserver = first\n", "server = bad!\nserver = first\n",
        "[server first]\nserver = first\n", "[other]\nserver = first\n", "[serverfirst]\n", "[myserver]\n",
        "[server first\n", "[server first]\n[server FIRST]\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        kz3_net_config_defaults(&cfg);
        assert(!kz3_net_parse_config("host = fallback.invalid\n", 24, &cfg, NULL));
        kz3_net_parse_config(invalid[i], strlen(invalid[i]), &cfg, NULL);
        assert(kz3_net_config_select(&cfg, NULL) == -1);
    }
    const char nul_line[] = {'s','e','r','v','e','r',0,'=','x','\n'};
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config(nul_line, sizeof(nul_line), &cfg, NULL) == 1);
    assert(kz3_net_config_select(&cfg, NULL) == -1);
    char long_line[520];
    memset(long_line, 'x', sizeof(long_line));
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config(long_line, sizeof(long_line), &cfg, NULL) == 1);
    assert(kz3_net_config_select(&cfg, NULL) == -1);
    kz3_net_config_defaults(&cfg);
    assert(kz3_net_parse_config("x", KZ3_NET_CONFIG_MAX + 1, &cfg, NULL) == 1);
    assert(kz3_net_config_select(&cfg, NULL) == -1);

    assert(kz3_net_valid_region_label("Valid \xC3\xA9"));
    assert(!kz3_net_valid_region_label("\xC0\xAF"));
    assert(!kz3_net_valid_region_label("\xED\xA0\x80"));
    char xml[64];
    assert(kz3_net_xml_escape("<&\"'>", xml, sizeof(xml)) > 0);
    assert(!strcmp(xml, "&lt;&amp;&quot;&apos;&gt;"));
    puts("PASS config defaults, selector validation and region handling");
}

static void test_ticket(void)
{
    enum { SIZE = 0x10000, VERSION = 0x10010, SERVICE = 0x10020, BUFFER = 0x11000 };
    assert(np_kz3_get_ticket(0, SIZE) < 0);
    ps3_net_set_online(0);
    assert(np_kz3_request_ticket2(0, 0, 0, 0, 0, 0, 0) < 0);
    np_kz3_sign_in("TestUser1");
    assert(!registrations && !identity_calls);
    ps3_net_set_online(1);
    np_kz3_sign_in("1invalid");
    assert(!registrations);
    np_kz3_sign_in("TestUser1");
    assert(registrations == 6 && identity_calls == 1 && ps3_net_np_signed_in());
    vm_write16(VERSION, 2); vm_write16(VERSION + 2, 1);
    memcpy(vm_base + SERVICE, "SYNTHETIC", 10);
    assert(np_kz3_request_ticket2(0, VERSION, SERVICE, 0, 1025, 0, 0) < 0 && !callbacks);
    assert(np_kz3_request_ticket2(0, VERSION, SERVICE, 0, 0, 0, 0) == 0);
    assert(callbacks == 1 && callback_opd == 0x18000u);
    assert(callback_args[0] == NP_KZ3_EVENT_GOT_TICKET && callback_args[1] == NP_TICKET_V21_SIZE);
    assert(callback_args[2] == 0x12345678u && !callback_args[3]);
    assert(np_kz3_get_ticket(0, SIZE) == 0 && vm_read32(SIZE) == NP_TICKET_V21_SIZE);
    assert(np_kz3_get_ticket(0, 0) < 0);
    vm_write32(SIZE, NP_TICKET_V21_SIZE - 1);
    assert(np_kz3_get_ticket(BUFFER, SIZE) < 0);
    vm_write32(SIZE, NP_TICKET_V21_SIZE);
    assert(np_kz3_get_ticket(BUFFER, SIZE) == 0);
    NpTicketInfo info;
    assert(!np_ticket_check(vm_base + BUFFER, NP_TICKET_V21_SIZE, &info));
    assert(!strcmp(info.online_id, "TestUser1") && !strcmp(info.service_id, "SYNTHETIC"));
    assert(info.version == 0x21010000u && info.body_entries >= 9);

    uint8_t synthetic[NP_TICKET_V21_SIZE];
    memcpy(synthetic, vm_base + BUFFER, sizeof(synthetic));
    assert(np_ticket_check(synthetic, 11, NULL) == NP_TICKET_E_SIZE);
    synthetic[0] = 0x11;
    assert(np_ticket_check(synthetic, sizeof(synthetic), NULL) == NP_TICKET_E_VERSION);
    synthetic[0] = 0x21;
    synthetic[4] ^= 1;
    assert(np_ticket_check(synthetic, sizeof(synthetic), NULL) == NP_TICKET_E_TRAILING);
    assert(np_kz3_set_ticket_file(synthetic, sizeof(synthetic), NULL) < 0);
    assert(!np_kz3_ticket_source_file());
    synthetic[4] ^= 1;
    synthetic[10] = 0xFF;
    assert(np_ticket_check(synthetic, sizeof(synthetic), NULL) == NP_TICKET_E_LENGTH);
    synthetic[10] = 0;
    synthetic[NP_TICKET_ONLINE_ID_OFF] = '1';
    assert(np_ticket_check(synthetic, sizeof(synthetic), NULL) == NP_TICKET_E_ONLINE_ID);
    synthetic[NP_TICKET_ONLINE_ID_OFF] = 'T';
    assert(!np_kz3_set_ticket_file(synthetic, sizeof(synthetic), NULL));
    assert(np_kz3_ticket_source_file() == sizeof(synthetic));
    synthetic[4] ^= 1;
    assert(np_kz3_set_ticket_file(synthetic, sizeof(synthetic), NULL) < 0);
    assert(np_kz3_ticket_source_file() == sizeof(synthetic));
    synthetic[4] ^= 1;
    callback_enabled = 0;
    assert(!np_kz3_request_ticket2(0, VERSION, SERVICE, 0, 0, 0, 0) && callbacks == 1);
    vm_write32(SIZE, sizeof(synthetic));
    assert(!np_kz3_get_ticket(BUFFER, SIZE));
    assert(!memcmp(vm_base + BUFFER, synthetic, sizeof(synthetic)));
    puts("PASS synthetic unsigned ticket shape, copy and callback");
}

static void test_rt(void)
{
    uint8_t hello[] = {0x91,0xA2,0xB3,0xC4,0xE5,0xF6,0x30,0x82,0,4,
                       0xDE,0xAD,0xBE,0xEF,0xF1,0xE2,0xD3,0xC4};
    RtFrame f = {0x24, 0, sizeof(hello), hello};
    uint8_t out[RTCAP_OUT];
    rt_log_frame(10071, 1, &f);
    assert(!rt_reply(&f, NULL, 0, out, sizeof(out)));
    const uint8_t answer[] = {1, 2, 3, 4};
    assert(!rt_reply(&f, answer, sizeof(answer), out, 6));
    assert(rt_reply(&f, answer, sizeof(answer), out, sizeof(out)) == 7);
    assert(out[0] == 0x25 && !memcmp(out + 3, answer, sizeof(answer)));

    uint8_t med[2 + MED_MSGID + MED_SKEY + NP_TICKET_V21_SIZE] = {4, 0x58};
    med[2] = 0xA1; med[3] = 0xB2; med[4] = 0xC3; med[5] = 0xD4;
    med[2 + MED_MSGID + MED_SKEY] = 0x21;
    med[3 + MED_MSGID + MED_SKEY] = 0x01;
    strcpy(s_cap_online_id, "TestUser1");
    f.id = 0x0B; f.len = sizeof(med); f.payload = med;
    rt_log_frame(10075, 2, &f);
    f.id = 0x55; f.len = sizeof(hello); f.payload = hello;
    rt_log_frame(10075, 2, &f);
    uint8_t connect[0x4C] = {0};
    connect[0] = 0xA1; connect[1] = 0xB2; connect[2] = 0xC3; connect[3] = 0xD4;
    connect[8] = 0xA1; connect[9] = 0xB2; connect[10] = 0xC3; connect[11] = 0xD4;
    connect[0x48] = 0xDE; connect[0x49] = 0xAD; connect[0x4A] = 0xBE; connect[0x4B] = 0xEF;
    f.id = 0; f.len = sizeof(connect); f.payload = connect;
    rt_log_frame(10071, 1, &f);

    uint8_t wire[] = {0x00, 2, 0, 0x11, 0x22, 0xA1, 3, 0, 1, 2, 3, 4, 0x33, 0x44, 0x55};
    RtFrame parsed;
    assert(!rt_frame_parse(wire, 2, &parsed));
    assert(!rt_frame_parse(wire, 4, &parsed));
    assert(rt_frame_parse(wire, sizeof(wire), &parsed) == 5 && parsed.id == 0 && parsed.len == 2);
    assert(!rt_frame_parse(wire + 5, 9, &parsed));
    assert(rt_frame_parse(wire + 5, 10, &parsed) == 10 && parsed.encrypted && parsed.id == 0x21);
    assert(!rt_reply(&parsed, answer, sizeof(answer), out, sizeof(out)));
    uint8_t empty[] = {0x24, 0, 0};
    for (size_t n = 0; n < sizeof(empty); n++) assert(!rt_frame_parse(empty, n, &parsed));
    assert(rt_frame_parse(empty, sizeof(empty), &parsed) == sizeof(empty) && !parsed.len);
    uint8_t enc_empty[] = {0xA4, 0, 0, 1, 2, 3, 4};
    for (size_t n = 0; n < sizeof(enc_empty); n++) assert(!rt_frame_parse(enc_empty, n, &parsed));
    assert(rt_frame_parse(enc_empty, sizeof(enc_empty), &parsed) == sizeof(enc_empty) && parsed.encrypted);
    static uint8_t maximum[3u + 0xFFFFu];
    maximum[0] = 0x55; maximum[1] = 0xFF; maximum[2] = 0xFF;
    assert(!rt_frame_parse(maximum, sizeof(maximum) - 1, &parsed));
    assert(rt_frame_parse(maximum, sizeof(maximum), &parsed) == sizeof(maximum));
    assert(parsed.len == 0xFFFFu && parsed.payload == maximum + 3);

    uint8_t session[2 + MED_MSGID] = {1, 0x03};
    f.id = 0x0B; f.encrypted = 0; f.payload = session; f.len = sizeof(session) - 1;
    assert(!rt_reply(&f, NULL, 0, out, sizeof(out)));
    f.len = sizeof(session);
    size_t response = rt_reply(&f, NULL, 0, out, sizeof(out));
    assert(response > 5 && out[0] == 0x0A && out[3] == 1 && out[4] == 0x04);

    uint8_t match[2 + 139 + 4] = {4, 0x88};
    match[2 + 139 - 4] = 4;
    match[2 + 139] = 0x9A; match[2 + 139 + 1] = 0xBC;
    match[2 + 139 + 2] = 0xDE; match[2 + 139 + 3] = 0xF0;
    f.payload = match; f.len = sizeof(match);
    response = rt_reply(&f, NULL, 0, out, sizeof(out));
    assert(response > 5 && out[0] == 0x0A && out[3] == 4 && out[4] == 0x89);

    uint8_t multi[] = {0x23,0,0, 0x21,0,0, 0xFE};
    f.id = 0x3B; f.encrypted = 1; f.len = sizeof(multi); f.payload = multi;
    sends = 0;
    rt_handle((rtcap_sock)0, 10075, 3, &f, out, 0);
    assert(!sends);
    f.encrypted = 0;
    rt_handle((rtcap_sock)0, 10075, 3, &f, out, 0);
    assert(sends == 2 && sent_ids[0] == 0x07 && sent_ids[1] == 0x1A);
    puts("PASS synthetic RT framing, replies and redacted metadata");
}

static void make_ticket(void)
{
    const char* path = getenv("KZ3_NET_TEST_TICKET");
    NpTicketFields fields = {0};
    uint8_t bytes[NP_TICKET_V21_SIZE];
    assert(path && *path);
    strcpy(fields.online_id, "TestUser1");
    strcpy(fields.service_id, "SYNTHETIC");
    assert(np_ticket_build(&fields, bytes, sizeof(bytes)) == sizeof(bytes));
    FILE* file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    assert(!fclose(file));
}

static void set_online_env(const char* value)
{
#ifdef _WIN32
    assert(!_putenv_s("KZ3_ONLINE", value));
#else
    assert(!setenv("KZ3_ONLINE", value, 1));
#endif
}

static void test_closed_registration(int expected_resolver_calls, int expected_ticket_opens,
                                     int expected_map_calls, int expected_allow_calls)
{
    const uint8_t old_ip[4] = {192, 0, 2, 47};
    uint8_t mapped[4];
    assert(ps3_net_online());
    assert(!ps3_net_policy_map_host(KZ3_NET_MUIS_HOST, old_ip));
    assert(!ps3_net_policy_allow(old_ip, 10071, 10071, PS3_NET_PROTO_TCP | PS3_NET_PROTO_UDP));
    ps3_net_set_np_signed_in(1);
    assert(kz3_net_register() == -1);
    assert(file_opens == 1 && ticket_opens == expected_ticket_opens && resolver_calls == expected_resolver_calls);
    assert(policy_map_calls == expected_map_calls && policy_allow_calls == expected_allow_calls);
    assert(!socket_calls && !sends && !registrations);
    assert(!np_kz3_ticket_source_file() && !ps3_net_np_signed_in() && !ps3_net_online());
    assert(ps3_net_policy_strict());
    assert(!ps3_net_policy_resolve(KZ3_NET_MUIS_HOST, mapped));
    assert(!ps3_net_policy_check(PS3_NET_PROTO_TCP, old_ip, 10071));
    assert(!ps3_net_policy_check(PS3_NET_PROTO_UDP, old_ip, 10071));
    puts("PASS failed setup refuses ticket, sign-in and stale destinations");
}

static void test_good_registration(void)
{
    const uint8_t ip[4] = {192, 0, 2, 47}, other[4] = {192, 0, 2, 48};
    uint8_t mapped[4];
    const char* expected = getenv("KZ3_NET_EXPECT_HOST");
    int ticket = getenv("KZ3_NET_EXPECT_TICKET") != NULL;
    int sign_in = getenv("KZ3_NET_EXPECT_NO_ID") == NULL;
    assert(ps3_net_online() && expected);
    assert(kz3_net_register() == 0);
    assert(file_opens == 1 && resolver_calls == 1 && !strcmp(resolved_name, expected));
    assert(policy_map_calls == 1 && policy_allow_calls == 2);
    assert(ticket_opens == ticket && !socket_calls && !sends);
    assert(registrations == (sign_in ? 6 : 0) && identity_calls == sign_in && ps3_net_policy_strict());
    assert(ps3_net_np_signed_in() == sign_in);
    assert(ps3_net_policy_resolve(KZ3_NET_MUIS_HOST, mapped) == 1 && !memcmp(mapped, ip, 4));
    assert(ps3_net_policy_check(PS3_NET_PROTO_TCP, ip, 10071));
    assert(!ps3_net_policy_check(PS3_NET_PROTO_UDP, ip, 53));
    assert(!ps3_net_policy_check(PS3_NET_PROTO_TCP, other, 10071));
    assert(np_kz3_ticket_source_file() == (ticket ? NP_TICKET_V21_SIZE : 0));
    puts("PASS selected synthetic server and strict registration");
}

int main(int argc, char** argv)
{
    static uint8_t memory[1u << 20];
    vm_base = memory;
    ppu_vm_size = sizeof(memory);
    assert(argc == 2);
    if (!strcmp(argv[1], "make-ticket")) make_ticket();
    else if (!strcmp(argv[1], "offline")) {
        assert(!ps3_net_online());
        assert(kz3_net_register() == 0);
        np_kz3_sign_in("TestUser1");
        assert(!file_opens && !ticket_opens && !resolver_calls && !socket_calls && !sends && !registrations);
        puts("PASS offline: no config, resolver, socket or NP registration");
    } else if (!strcmp(argv[1], "bad")) test_closed_registration(0, 0, 0, 0);
    else if (!strcmp(argv[1], "unresolved")) test_closed_registration(1, 0, 0, 0);
    else if (!strcmp(argv[1], "bad-ticket")) test_closed_registration(1, 1, 0, 0);
    else if (!strcmp(argv[1], "policy-failure")) test_closed_registration(1, 0, 1, 2);
    else if (!strcmp(argv[1], "good")) test_good_registration();
    else if (!strcmp(argv[1], "functional")) { test_config(); test_ticket(); }
    else if (!strcmp(argv[1], "rt")) test_rt();
    else if (!strcmp(argv[1], "cache-off")) {
        assert(!ps3_net_online());
        set_online_env("1");
        assert(!ps3_net_online());
        ps3_net_set_online(1);
        assert(ps3_net_online());
        puts("PASS offline switch cached until explicitly changed");
    } else if (!strcmp(argv[1], "cache-on")) {
        assert(ps3_net_online());
        set_online_env("0");
        assert(ps3_net_online());
        ps3_net_set_online(0);
        assert(!ps3_net_online());
        puts("PASS online switch cached until explicitly changed");
    } else assert(!"unknown mode");
    return 0;
}
