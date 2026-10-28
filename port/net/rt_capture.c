/*
 * KZ3 port, test only: loopback capture of the game's first RT frames (net plan step 5, the RE notes sections 9-10).
 *
 * Included by kz3_net.c. Runs only when the player is online (KZ3_ONLINE), KZ3_NET_CAPTURE is set and the server is
 * 127.0.0.0/8: a thread inside the game process listens on 127.0.0.1:10071 (never any other address), accepts the
 * game's MUIS connection and logs each RT frame (id, length, payload as hex up to 96 bytes) plus the decoded
 * CLIENT_HELLO parameters and CLIENT_CONNECT_TCP fields. The access token of CONNECT_TCP is logged as its length
 * and an FNV-1a hash only. It answers only what is needed to get those bytes:
 *   CLIENT_HELLO (0x24)       -> SERVER_HELLO (0x25) with the payload of KZ3_NET_CAPTURE_HELLO (a file the tester
 *                                provides; nothing is sent without it)
 *   CLIENT_CONNECT_TCP (0x00) -> SERVER_CONNECT_REQUIRE (0x22) {0, 584 LE} (the default of a self-hosted server)
 *   CLIENT_CONNECT_READY_REQUIRE (0x23) -> SERVER_CONNECT_ACCEPT_TCP (0x07); CLIENT_CONNECT_READY_TCP (0x21) ->
 *                                SERVER_CONNECT_COMPLETE (0x1A) (pass 5, section 10: the first Medius request follows)
 * Pass 6 (section 11), Medius messages in CLIENT_APP_TOSERVER (0x0B), answered in SERVER_APP (0x0A):
 *   1/0xC8 GetUniverseInformation -> 4/0x1E UniverseVariableSvoURLResponse (RTCAP_SVO_URL, when asked for) and
 *                                4/0x11 UniverseVariableInformationResponse: one universe whose DNS is the MUIS host
 *                                name (mapped to the server by the egress policy) and port the MAS listener
 *   1/0x03 or 4/0x8B SessionBegin -> 1/0x04 SessionBeginResponse (success, session key RTCAP_SESSION_KEY)
 *   4/0x7B SetLocalizationParams1 -> 1/0xA4 SetLocalizationParamsResponse (success)
 *   1/0x8C GetLocations        -> 1/0x8D GetLocationsResponse (one location, success, end of list)
 *   1/0x05 SessionEnd          -> 1/0x06 SessionEndResponse (success)
 *   4/0x58 TicketLogin         -> 4/0x59 TicketLoginResponse (pass 8: success, account 1, lobby world 1, the MLS
 *                                 at 127.0.0.1:10078); the ticket is logged as its offset only
 * Pass 8 (section 13), on the MLS listener 10078: 1/0x86 VersionServer -> 1/0x87; 4/0x5D -> 4/0x5E (empty list);
 *   4/0x1F ChannelList_ExtraInfo -> 1/0xED (one lobby); 1/0x25 JoinChannel -> 1/0x26 (the same MLS);
 *   1/0x31 PlayerInfo -> 1/0x32; 1/0x49 UpdateUserState unanswered (as the server model).
 * Pass 10 (section 15): 4/0x65 party create -> 4/0x66 (success, world 2); 1/0x37 FindWorldByName -> 1/0x38 (the
 *   party world); 4/0x87 party join -> 4/0x8E (NetConnectionInfo of the DME listener 10079, started like the MLS);
 *   on any listener CLIENT_TOKEN_MESSAGE (0x2F) type 2 -> SERVER_TOKEN_MESSAGE (0x30) type 8 (granted); 4/0x88
 *   (match type picked) -> 4/0x89 status 11 (accepted). Pass 13 (section 16): right behind 4/0x89 the push
 *   4/0x8A (match found: a game world and the game host at the DME listener), keyed with the request counter and
 *   the party world; 4/0x35 GameInfo -> 4/0x36 (success, "Loopback match"); 1/0xF3 JoinGame -> 1/0x24 (the DME
 *   listener again, world 3); 4/0x2A GetServerTime -> 4/0x2B (the host's clock, GMT); 4/0x2F CreateGame -> 1/0x1E
 *   (world 3; the 4/0x8A record says world -1 = host unless KZ3_NET_CAPTURE_JOIN=1); RT 0x2D -> 0x2E (timebase).
 * Pass 7 (section 12): RT 0x3B CLIENT_MULTI_APP_TOSERVER is split into its RT parts, each handled as a frame (KZ3
 * sends DnasSignaturePost 4/0x0A, logged without its signature, and TicketLogin in one 0x3B after the region pick).
 * Four listeners, MUIS, MAS and MLS (kz3_net_capture_start once per port) and the SVO HTTP logger on 10070
 * (kz3_net_capture_start_kind(..., 1): request line without query values, header names; GET
 * .../kin/stats/region_list.jsp answered with a one-region XML list, pass 7; kin/account/nplogin.do with a login
 * result of 30000, pass 9; everything else 404),
 * one accept thread each and one thread per connection (pass 8).
 * A hello's certificate is logged as offset, length and hash; a TicketLogin (Medius 4/0x58) as its header and the
 * ticket's offset, never the ticket. Every other frame is logged and left unanswered. RT framing: u8 id (bit 7 = encrypted, then a 4-byte hash), u16
 * little-endian payload length, payload. All bytes from the peer are bounds-checked before use.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#  include <strings.h>
#endif
#include <string.h>

#ifdef _WIN32
typedef SOCKET rtcap_sock;
#  define RTCAP_BAD INVALID_SOCKET
#  define rtcap_close closesocket
#else
#  include <arpa/inet.h>
#  include <pthread.h>
#  include <unistd.h>
typedef int rtcap_sock;
#  define RTCAP_BAD (-1)
#  define rtcap_close close
#endif

#define RTCAP_HELLO_MAX   0x2000u
#define RTCAP_LOG_MAX     8192u      /* payload bytes logged per connection */
#define RTCAP_BUF         (3u + 4u + 0xFFFFu)
#define RTCAP_OUT         (3u + RTCAP_HELLO_MAX)
#define RTCAP_SESSION_KEY "kz3capture"
#define RTCAP_UNIVERSE    "KZ3 loopback capture"
#define RTCAP_LOCATION    "Loopback"
/* SVO URL in the universe answer: the MUIS name (cellHttp resolves through the policy map, which refuses a literal
 * address) and port 10070, where the capture's HTTP logger listens (pass 6) */
#define RTCAP_SVO_URL     "http://" KZ3_NET_MUIS_HOST ":10070/"

/* Medius field sizes (protocol facts, the RE notes section 11) */
#define MED_MSGID   21u
#define MED_SKEY    17u
#define MED_UNAME   128u     /* universe name */
#define MED_UDNS    128u
#define MED_UDESC   256u
#define MED_BSP     8u
#define MED_BSPNAME 128u
#define MED_UEXTRA  128u
#define MED_LOCNAME 64u
#define MED_AKEY    17u      /* NetConnectionInfo access key */
#define MED_NETADDR 16u      /* NetAddress string */
#define MED_RSAKEY  64u      /* RSA_KEY, 16 u32 */
#define MED_VERSION 56u      /* VersionServerResponse string */
#define MED_LOBBYNAME 64u
#define MED_ACCOUNTNAME 32u
#define MED_STATS   256u

/* The lobby (MLS) the TicketLogin answer points at (Horizon's default MLS port; in the policy's default 10070-10080),
 * and the NAT service address of the same answer (Horizon's default NATPort 10070, UDP; nothing listens) */
#define RTCAP_MLS_PORT    10078u
#define RTCAP_NAT_PORT    10070u
#define RTCAP_ACCESS_KEY  "kz3access"
#define RTCAP_ACCOUNT_ID  1u
#define RTCAP_LOBBY_WORLD 1u
#define RTCAP_LOBBY_NAME  "Loopback lobby"
#define RTCAP_PARTY_WORLD 2u            /* the party's game world (4/0x66, 1/0x38, 4/0x8E) */
#define RTCAP_DME_PORT    10079u        /* the game host (DME) listener the 4/0x8E answer points at (Horizon's default) */
#define RTCAP_APP_ID      23044u        /* KZ3's application id (CONNECT_TCP) */
#define MED_WORLDNAME     64u
#define MED_APPNAME       32u
#define MED_GAMEPW        32u
#define RTCAP_VERSION     "KZ3 loopback capture MLS"   /* ASSUMPTION: the client only shows or logs it */

typedef struct {
    uint8_t        id;           /* without bit 7 */
    int            encrypted;
    uint16_t       len;          /* payload bytes */
    const uint8_t* payload;
} RtFrame;

/* Bytes of the first complete frame in buf[0..n) (0 = need more). */
static size_t rt_frame_parse(const uint8_t* buf, size_t n, RtFrame* f)
{
    if (n < 3) return 0;
    size_t hdr = (buf[0] & 0x80) ? 7 : 3;
    uint16_t len = (uint16_t)(buf[1] | buf[2] << 8);
    if (n < hdr + len) return 0;
    f->id = buf[0] & 0x7F;
    f->encrypted = (buf[0] & 0x80) != 0;
    f->len = len;
    f->payload = buf + hdr;
    return hdr + len;
}

static uint32_t rt_fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32c(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static char s_cap_online_id[33];   /* the configured online id, compared with the ticket's field (never logged) */

/* Message offset (class and type included) of a TicketLogin's ticket: its version word ("21 01 00 00", "31 00 00 00"
 * or "41 00 00 00"), searched after MessageID and SessionKey (a MessageID "1" or a key ending in '1' looks like one);
 * 0 = not found. */
static size_t rt_ticket_at(const uint8_t* p, size_t len)
{
    for (size_t i = 2 + MED_MSGID + MED_SKEY; i + 4 <= len; i++)
        if (((p[i] == 0x21 && p[i + 1] == 0x01) || p[i] == 0x31 || p[i] == 0x41) && !p[i + 2] && !p[i + 3]) return i;
    return 0;
}

/* One line describing a frame's decoded fields ("" when the type is not decoded). */
static void rt_describe(const RtFrame* f, char* out, size_t cap)
{
    out[0] = 0;
    if (f->encrypted) { snprintf(out, cap, "encrypted (not decoded)"); return; }
    if (f->id == 0x24) {
        char* o = out;
        size_t left = cap;
        int w = snprintf(o, left, "CLIENT_HELLO params (u16 LE):");
        for (size_t i = 0; w > 0 && (size_t)w < left && i + 1 < f->len && i < 14; i += 2) {
            o += w; left -= (size_t)w;
            w = snprintf(o, left, " %u", (unsigned)(f->payload[i] | f->payload[i + 1] << 8));
        }
        /* A DER certificate (30 82 hi lo) after the parameters: its length and hash, then the bytes after it. */
        const uint8_t* p = f->payload;
        for (size_t c = 6; w > 0 && (size_t)w < left && c + 4 <= f->len && c <= 32; c++) {
            if (p[c] != 0x30 || p[c + 1] != 0x82) continue;
            size_t cl = 4u + ((size_t)p[c + 2] << 8 | p[c + 3]);
            if (c + cl > f->len) break;
            o += w; left -= (size_t)w;
            w = snprintf(o, left, " | certificate at %u, %u bytes, fnv %08x | tail %u bytes:", (unsigned)c, (unsigned)cl,
                         rt_fnv1a(p + c, cl), (unsigned)(f->len - c - cl));
            for (size_t i = c + cl; w > 0 && (size_t)w < left && i < f->len && i < c + cl + 16; i++) {
                o += w; left -= (size_t)w;
                w = snprintf(o, left, " %02x", p[i]);
            }
            break;
        }
    } else if (f->id == 0x0B && f->len >= 2) {
        /* CLIENT_APP_TOSERVER: a Medius message, class and type first. For TicketLogin (4/0x58) the offset of the
         * ticket (its "21 01 00 00" / "31 00 00 00" / "41 00 00 00" version word) in the message, not its bytes. */
        const uint8_t* p = f->payload;
        int w = snprintf(out, cap, "CLIENT_APP_TOSERVER medius class %u type 0x%02X, %u bytes", p[0], p[1], (unsigned)f->len);
        if (p[0] == 4 && p[1] == 0x58 && w > 0 && (size_t)w < cap) {
            size_t at = rt_ticket_at(p, f->len);
            if (at) {
                size_t uid = at - 2 + 0x54;   /* body offset (after class and type) of the ticket's online id field */
                const char* match = "not checked";
                if (s_cap_online_id[0] && at + 0x54 + 32 <= f->len)
                    match = (!strncmp((const char*)p + at + 0x54, s_cap_online_id, 32)) ? "yes" : "no";
                snprintf(out + w, cap - (size_t)w,
                         ", ticket at message offset %u = body offset %u (u32 before it: LE %u, BE %u), %u frame bytes "
                         "from the ticket on, online id field at body offset %u (matches online_id: %s)",
                         (unsigned)at, (unsigned)(at - 2), le32(p + at - 4), be32c(p + at - 4),
                         (unsigned)(f->len - at), (unsigned)uid, match);
            } else snprintf(out + w, cap - (size_t)w, ", no ticket version word found");
        }
    } else if (f->id == 0x00 && f->len >= 8) {
        const uint8_t* p = f->payload;
        int key_zero = 1;
        size_t key_end = f->len < 0x48 ? f->len : 0x48;
        for (size_t i = 8; i < key_end; i++) if (p[i]) key_zero = 0;
        size_t tok = f->len > 0x48 ? f->len - 0x48u : 0;
        snprintf(out, cap,
                 "CLIENT_CONNECT_TCP world %u (BE %u), app id %u (BE %u), 0x40-byte key field %s%s, token %u bytes fnv %08x",
                 le32(p), be32c(p), le32(p + 4), be32c(p + 4),
                 f->len < 0x48 ? "short " : "", key_zero ? "all zero" : "non-zero",
                 (unsigned)tok, tok ? rt_fnv1a(p + 0x48, tok) : 0u);
    }
}

/* ---- Medius encoders (pass 6). Each writes into out[0..cap) and returns the bytes written, 0 when it does not fit.
 * Integers little-endian, strings fixed-size and NUL-padded (at most size-1 characters kept). ---- */

static uint16_t s_cap_mas_port = 10075;   /* the universe's port in the GetUniverseInformation answer */

typedef struct { uint8_t* p; size_t n, cap; int bad; } MedW;

static void medw_bytes(MedW* w, const void* src, size_t n)
{
    if (w->bad || n > w->cap - w->n) { w->bad = 1; return; }
    if (src) memcpy(w->p + w->n, src, n); else memset(w->p + w->n, 0, n);
    w->n += n;
}
static void medw_u32(MedW* w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    medw_bytes(w, b, 4);
}
static void medw_str(MedW* w, const char* s, size_t size)
{
    size_t l = s ? strlen(s) : 0;
    if (l > size - 1) l = size - 1;
    if (w->bad || size > w->cap - w->n) { w->bad = 1; return; }
    memset(w->p + w->n, 0, size);
    if (l) memcpy(w->p + w->n, s, l);
    w->n += size;
}

/* RT SERVER_APP (0x0A) frame around one Medius message: u8 0x0A, u16 LE length, class, type, body. */
static size_t med_frame(uint8_t cls, uint8_t type, const uint8_t* body, size_t blen, uint8_t* out, size_t cap)
{
    if (blen > 0xFFFDu || cap < 5 || blen > cap - 5) return 0;
    out[0] = 0x0A; out[1] = (uint8_t)(blen + 2); out[2] = (uint8_t)((blen + 2) >> 8);
    out[3] = cls; out[4] = type;
    memcpy(out + 5, body, blen);
    return blen + 5;
}

/* Pass 13 (the RE notes 16): the server push 4/0x8A that ends KZ3's "Joining game" wait, built from the client's
 * 4/0x88 (p = class, type, body; len bounded by the frame). 4/0x88 (coder 0xA14F60, 139 bytes + lists): MessageID[21],
 * SessionKey[17], u32 (params +0xC), u32, char[32], u32, u32 (params +0x10), u32 (params +0x14), char[4] (flags, 0x20
 * set), u32 (params +0x18 as 0/1), char[17], char[16], u32 n, u32 cookie length, n x u32 (the params' u32 list from
 * its second entry), cookie bytes (the params' byte list with the request counter 0x14DF138 inserted in front).
 * 4/0x8A (decoder 0xA163C8): char[16] key, u32, u32, i32 result, u32 reason, then when result is 6: u32 size and one
 * game record (decoder 0xA15C74, 484 bytes + data on the wire; the size field counts the in-memory
 * 0x1E8 + data): u32 world id, u32, u32, name[64], char[256], 15 x u32, char[32],
 * u32 (+0x1A8), u32 (+0x1E4), NetAddressList (2 x {u32 type, char[16], u32 port}), u32 data length, data.
 * The game's handler 0x16074C (state 2 "Joining game") takes it when key[0] is the request counter (the cookie's
 * first byte) and key[1..4] (LE) is the params' +8 word, which is not in the request: run net-r13-c read it from
 * guest memory as 2, the party's world id (4/0x66, 4/0x87), which the server knows; it then keeps the world id
 * (+0x18), key[5..8] (+0x5C), the record's +0x1E4 (+0x1C) and its data (+0x20) and goes to state 4 (match found).
 * Result 1 with reason 6 -> error -948, result 1 otherwise or -927 -> -977, others -> error. Every length from the
 * request is checked against len before use; key[5..15] stay 0 (key[5..8] -> matchmaker +0x5C, unknown). */
/* Test-only diagnosis (KZ3_NET_CAPTURE_PEEK=1, v1.00 addresses): logs what the game's 4/0x8A handler 0x16074C will
 * compare, read from guest memory: the matchmaker 0x134086C (state +4, key word +0x58) and the request counter
 * 0x14DF138. Never used to build an answer (a server cannot see them). */
extern uint8_t* vm_base;
static uint32_t rt_guest32(uint32_t ea) { uint8_t* b = vm_base + ea; return be32c(b); }
static void rt_peek_matchmaker(void)
{
    const char* e = getenv("KZ3_NET_CAPTURE_PEEK");
    if (!e || strcmp(e, "1") || !vm_base) return;
    uint32_t mm = rt_guest32(0x134086Cu);
    if (mm < 0x10000u || mm > 0xFFFFFF00u) { printf("[kz3net-cap]   peek: no matchmaker (0x%08x)\n", mm); return; }
    printf("[kz3net-cap]   peek: matchmaker 0x%08x state %u key word 0x%08x counter %u\n", mm, rt_guest32(mm + 4),
           rt_guest32(mm + 0x58), vm_base[0x14DF138u]);
}
#define RTCAP_MATCH_WORLD 3u            /* the match's game world in the 4/0x8A record */
static size_t med_match_found(const uint8_t* p, size_t len, uint8_t* out, size_t cap)
{
    const size_t fixed = 2 + 139;
    if (len < fixed) return 0;
    uint32_t n = le32(p + fixed - 8), clen = le32(p + fixed - 4);
    if (n > 64 || clen > 4096 || (size_t)n * 4 > len - fixed || clen > len - fixed - (size_t)n * 4) return 0;
    const uint8_t* cookie = p + fixed + (size_t)n * 4;
    uint8_t key[16] = { 0 };
    if (clen) key[0] = cookie[0];             /* the request counter */
    key[1] = (uint8_t)RTCAP_PARTY_WORLD; key[2] = (uint8_t)(RTCAP_PARTY_WORLD >> 8);   /* LE: the party's world id */
    key[3] = (uint8_t)(RTCAP_PARTY_WORLD >> 16); key[4] = (uint8_t)(RTCAP_PARTY_WORLD >> 24);
    printf("[kz3net-cap]   4/0x88: %u list words, cookie %u bytes, key", (unsigned)n, (unsigned)clen);
    for (size_t i = 0; i < sizeof(key); i++) printf(" %02x", key[i]);
    printf("\n");
    rt_peek_matchmaker();
    MedW w = { out, 0, cap, 0 };
    medw_bytes(&w, key, sizeof(key));
    medw_u32(&w, 0);
    medw_u32(&w, 0);
    medw_u32(&w, 6);                          /* result 6: a game was found, the record follows */
    medw_u32(&w, 0);
    medw_u32(&w, 0x1E8);                      /* record size in memory (0xA24E04: 0x1E8 + data length), no data */
    const char* j = getenv("KZ3_NET_CAPTURE_JOIN");   /* test variant: join world 3 instead of hosting */
    medw_u32(&w, (j && !strcmp(j, "1")) ? RTCAP_MATCH_WORLD : 0xFFFFFFFFu);   /* -1: this client hosts (16.2) */
    medw_u32(&w, 0);
    medw_u32(&w, 0);
    medw_str(&w, "Loopback match", MED_WORLDNAME);
    medw_bytes(&w, NULL, MED_STATS);
    for (int i = 0; i < 15; i++) medw_u32(&w, 0);   /* ASSUMPTION: player counts, level, rules; unread by 0x16074C */
    medw_bytes(&w, NULL, 32);
    medw_u32(&w, 0);                          /* +0x1A8. ASSUMPTION: game host type 0 (client-server) */
    medw_u32(&w, 0);                          /* +0x1E4 -> matchmaker +0x1C */
    medw_u32(&w, 1);                          /* NetAddressTypeExternal: the game host */
    medw_str(&w, "127.0.0.1", MED_NETADDR);
    medw_u32(&w, RTCAP_DME_PORT);
    medw_u32(&w, 3);                          /* NetAddressTypeNATService */
    medw_str(&w, "127.0.0.1", MED_NETADDR);
    medw_u32(&w, RTCAP_NAT_PORT);
    medw_u32(&w, 0);                          /* data length */
    return w.bad ? 0 : w.n;
}

/* Body of UniverseVariableInformationResponse (4/0x11): MessageID[21], i32 status, u32 filter, then per filter bit:
 * ID u32; NAME[128]; DNS[128] + i32 port; DESCRIPTION[256]; STATUS i32 status, users, max users; BILLING [8] + [128];
 * EXTRAINFO [128]; (SVO_URL travels in its own message, nothing here); then u8 end-of-list + 3 pad. */
static size_t med_universe_info(const uint8_t* msgid, uint32_t filter, uint32_t id, const char* name, const char* dns,
                                uint32_t port, int end_of_list, uint8_t* out, size_t cap)
{
    MedW w = { out, 0, cap, 0 };
    medw_bytes(&w, msgid, MED_MSGID);
    medw_u32(&w, 0);                                    /* MediusSuccess */
    medw_u32(&w, filter);
    if (filter & (1u << 2)) medw_u32(&w, id);
    if (filter & (1u << 3)) medw_str(&w, name, MED_UNAME);
    if (filter & (1u << 4)) { medw_str(&w, dns, MED_UDNS); medw_u32(&w, port); }
    if (filter & (1u << 5)) medw_str(&w, "", MED_UDESC);
    /* STATUS: i32 status, users, max users. KZ3's universe callback 0x162C2C keeps (status == 0) as a flag per entry
     * (ASSUMPTION: "down"); a default Horizon universe sends 0, 0, 0. The capture says up (1), 0 of 256. */
    if (filter & (1u << 6)) { medw_u32(&w, 1); medw_u32(&w, 0); medw_u32(&w, 256); }
    if (filter & (1u << 7)) { medw_str(&w, "", MED_BSP); medw_str(&w, "", MED_BSPNAME); }
    if (filter & (1u << 8)) medw_str(&w, "", MED_UEXTRA);
    uint8_t eol[4] = { (uint8_t)(end_of_list ? 1 : 0), 0, 0, 0 };
    medw_bytes(&w, eol, 4);
    return w.bad ? 0 : w.n;
}

/* Body of a status-only answer: MessageID[21], 3 pad, i32 status (SetLocalizationParamsResponse 1/0xA4), optionally
 * followed by SessionKey[17] + 3 pad (SessionBeginResponse 1/0x04). */
static size_t med_status(const uint8_t* msgid, const char* session_key, uint8_t* out, size_t cap)
{
    MedW w = { out, 0, cap, 0 };
    medw_bytes(&w, msgid, MED_MSGID);
    medw_bytes(&w, NULL, 3);
    medw_u32(&w, 0);
    if (session_key) { medw_str(&w, session_key, MED_SKEY); medw_bytes(&w, NULL, 3); }
    return w.bad ? 0 : w.n;
}

/* NetConnectionInfo (156 bytes, KZ3 decoder 0x9EFF08) pointing at the capture's MLS: i32 type, 2 x NetAddress
 * {i32 type, char[16], u32 port}, i32 world id, RSA_KEY[64], SessionKey[17], AccessKey[17], 2 pad. */
static void med_net_connection_info_to(MedW* w, uint32_t port, uint32_t world)
{
    medw_u32(w, 1);                          /* NetConnectionTypeClientServerTCP */
    medw_u32(w, 1);                          /* NetAddressTypeExternal: the server */
    medw_str(w, "127.0.0.1", MED_NETADDR);
    medw_u32(w, port);
    medw_u32(w, 3);                          /* NetAddressTypeNATService */
    medw_str(w, "127.0.0.1", MED_NETADDR);
    medw_u32(w, RTCAP_NAT_PORT);
    medw_u32(w, world);
    medw_bytes(w, NULL, MED_RSAKEY);         /* no server key (Horizon sends an empty one for TicketLogin too) */
    medw_str(w, RTCAP_SESSION_KEY, MED_SKEY);
    medw_str(w, RTCAP_ACCESS_KEY, MED_AKEY);
    medw_bytes(w, NULL, 2);
}

static void med_net_connection_info(MedW* w) { med_net_connection_info_to(w, RTCAP_MLS_PORT, RTCAP_LOBBY_WORLD); }

/* Body of TicketLoginResponse 4/0x59 (pass 8, the RE notes section 13.1), as KZ3's decoder 0xA15A98 reads it
 * (packed, no alignment): MessageID[21], i32 StatusCodeTicketLogin, i32 PasswordType, then an embedded
 * AccountLoginResponse (decoder 0xA159D4, 0xC4 bytes in memory): MessageID[21], 3 pad, i32 status, i32 AccountID,
 * i32 AccountType, i32 MediusWorldID, NetConnectionInfo (decoder 0x9EFF08): i32 type, 2 x NetAddress {i32 type,
 * char[16], u32 port}, i32 world id, RSA_KEY[64], SessionKey[17], AccessKey[17], 2 pad. 225 bytes. Horizon writes
 * 3 pad + status + 29 zero bytes instead of the first two i32 and the inner header: the same bytes on success. */
static size_t med_ticket_login_response(const uint8_t* msgid, uint8_t* out, size_t cap)
{
    MedW w = { out, 0, cap, 0 };
    medw_bytes(&w, msgid, MED_MSGID);
    medw_u32(&w, 0);                         /* StatusCodeTicketLogin: MediusSuccess */
    medw_u32(&w, 0);                         /* PasswordType (ASSUMPTION: 0 = not set) */
    medw_bytes(&w, NULL, MED_MSGID + 3);     /* inner MessageID (Horizon: empty), 3 pad */
    medw_u32(&w, 0);                         /* inner status; 5 takes another branch in the callback 0xA1AEEC */
    medw_u32(&w, RTCAP_ACCOUNT_ID);
    medw_u32(&w, 1);                         /* MediusMasterAccount */
    medw_u32(&w, RTCAP_LOBBY_WORLD);
    med_net_connection_info(&w);
    return w.bad ? 0 : w.n;
}

/* Answers to a Medius message (payload of CLIENT_APP_TOSERVER: class, type, body) as RT frames in out[0..cap). */
static size_t med_reply(const uint8_t* p, size_t len, uint8_t* out, size_t cap)
{
    uint8_t body[1024];
    if (len < 2 + MED_MSGID) return 0;
    const uint8_t* msgid = p + 2;
    uint8_t cls = p[0], type = p[1];
    if (cls == 1 && type == 0xC8 && len >= 2 + MED_MSGID + 3 + 4) {           /* GetUniverseInformation */
        uint32_t filter = le32(p + 2 + MED_MSGID + 3);
        size_t n = 0, b;
        if (filter & (1u << 9)) {   /* SVO URL asked: RTCAP_SVO_URL, 1-byte length. Its MessageID must be the request's:
                                     * KZ3's 4/0x11 handler 0xA1A8FC compares the two and fails with MediusFail
                                     * (-966) otherwise (run net-r6-mas2 with an empty id; section 11) */
            uint8_t svo[MED_MSGID + 1 + sizeof(RTCAP_SVO_URL)];
            memcpy(svo, msgid, MED_MSGID);
            svo[MED_MSGID] = (uint8_t)sizeof(RTCAP_SVO_URL);          /* length with the NUL, then the URL + NUL */
            memcpy(svo + MED_MSGID + 1, RTCAP_SVO_URL, sizeof(RTCAP_SVO_URL));
            if (!(b = med_frame(4, 0x1E, svo, sizeof(svo), out, cap))) return 0;
            n += b;
        }
        b = med_universe_info(msgid, filter, 1, RTCAP_UNIVERSE, KZ3_NET_MUIS_HOST, s_cap_mas_port, 1,
                              body, sizeof(body));
        if (!b || !(b = med_frame(4, 0x11, body, b, out + n, cap - n))) return 0;
        return n + b;
    }
    if ((cls == 1 && type == 0x03) || (cls == 4 && type == 0x8B)) {          /* SessionBegin / SessionBegin1 */
        size_t b = med_status(msgid, RTCAP_SESSION_KEY, body, sizeof(body));
        return b ? med_frame(1, 0x04, body, b, out, cap) : 0;
    }
    if (cls == 1 && type == 0x8C) {   /* GetLocations (sent on MAS before TicketLogin) -> 1/0x8D GetLocationsResponse:
                                       * MessageID[21], 3 pad, i32 location id, name[64], i32 status, u8 end, 3 pad */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 1);
        medw_str(&w, RTCAP_LOCATION, MED_LOCNAME);
        medw_u32(&w, 0);
        uint8_t eol[4] = { 1, 0, 0, 0 };
        medw_bytes(&w, eol, 4);
        return w.bad ? 0 : med_frame(1, 0x8D, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0x05) {                                           /* SessionEnd -> 1/0x06 (success) */
        size_t b = med_status(msgid, NULL, body, sizeof(body));
        return b ? med_frame(1, 0x06, body, b, out, cap) : 0;
    }
    if (cls == 4 && type == 0x58) {                                           /* TicketLogin -> 4/0x59 */
        size_t b = med_ticket_login_response(msgid, body, sizeof(body));
        return b ? med_frame(4, 0x59, body, b, out, cap) : 0;
    }
    if (cls == 4 && type == 0x5D) {   /* pass 8, second message on the MLS (MessageID + SessionKey; no Horizon model,
                                       * name unknown) -> 4/0x5E as KZ3's decoder 0xA15684 reads it (592 bytes):
                                       * MessageID[21], i32 status, u8 end-of-list, i32 id, char[50], char[256],
                                       * char[256]. A list: the game's callback 0x1647C0 appends each id on status
                                       * 0 and stops at end-of-list 1, or at status 1 (NoResult, empty list).
                                       * Pass 8 run net-r8-c sent status 0 with end 0: the client kept waiting. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_u32(&w, 1);                     /* MediusNoResult: an empty list */
        uint8_t eol = 1;
        medw_bytes(&w, &eol, 1);
        medw_bytes(&w, NULL, 4 + 50 + 256 + 256);
        return w.bad ? 0 : med_frame(4, 0x5E, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x1F) {   /* ChannelList_ExtraInfo (pass 8, third on the MLS) -> 1/0xED, one lobby, as
                                       * KZ3's decoder 0xA23708 reads it (= Horizon's v>108 model, 132 bytes):
                                       * MessageID[21], 3 pad, i32 status, i32 world id, u16 players, u16 max,
                                       * u16 game worlds, 2 pad, i32 security level, 4 x u32 generic fields, i32
                                       * generic field level, LobbyName[64], u8 end-of-list, 3 pad.
                                       * ASSUMPTION: generic fields 0 are acceptable to KZ3 (not checked yet). */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, RTCAP_LOBBY_WORLD);
        uint8_t counts[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };   /* 0 players, max 256, 0 game worlds, pad */
        medw_bytes(&w, counts, sizeof(counts));
        medw_bytes(&w, NULL, 4 + 16 + 4);
        medw_str(&w, RTCAP_LOBBY_NAME, MED_LOBBYNAME);
        uint8_t eol[4] = { 1, 0, 0, 0 };
        medw_bytes(&w, eol, 4);
        return w.bad ? 0 : med_frame(1, 0xED, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0x25) {   /* JoinChannel -> 1/0x26 (KZ3 decoder 0xA21E94, 184 bytes): MessageID[21],
                                       * 3 pad, i32 status, NetConnectionInfo (the same MLS, as Horizon answers).
                                       * Prepared in pass 8 before the client was seen sending it. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        med_net_connection_info(&w);
        return w.bad ? 0 : med_frame(1, 0x26, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0x31) {   /* PlayerInfo (after the channel join, pass 8) -> 1/0x32 (KZ3 decoder 0xA1D24C,
                                       * = Horizon's model, 328 bytes): MessageID[21], 3 pad, i32 status,
                                       * AccountName[32], i32 application id, i32 player status, i32 connection
                                       * class, Stats[256]. The account name is the configured online id. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_str(&w, s_cap_online_id[0] ? s_cap_online_id : "Player", MED_ACCOUNTNAME);
        medw_u32(&w, 23044);                 /* KZ3's application id (CONNECT_TCP) */
        medw_u32(&w, 2);                     /* MediusPlayerInChatWorld (in a lobby) */
        medw_u32(&w, 1);                     /* Ethernet */
        medw_bytes(&w, NULL, MED_STATS);     /* ASSUMPTION: empty stats are acceptable */
        return w.bad ? 0 : med_frame(1, 0x32, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0x86) {   /* VersionServer (first message on the MLS, pass 8) -> 1/0x87: MessageID[21],
                                       * VersionServer[56], no status (KZ3's decoder 0xA1B408, 77 bytes) */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_str(&w, RTCAP_VERSION, MED_VERSION);
        return w.bad ? 0 : med_frame(1, 0x87, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x65) {   /* pass 10 (section 15), first MLS message in the MULTIPLAYER menu: KZ3's
                                       * party create (sender 0x153db4, coder 0xA14790, 201 bytes after the class/id:
                                       * MessageID, SessionKey, 2 pad, 3 x u32, name[64] "PP_<online id>", char[32],
                                       * 9 x u32, char[17]; no Horizon model) -> 4/0x66 (coder 0xA14714, 32 bytes):
                                       * MessageID[21], 3 pad, i32 status, u32 world id. The game callback 0x154c6c
                                       * accepts status 0 or -975 (MediusGameNameExists); others are an error. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, RTCAP_PARTY_WORLD);
        return w.bad ? 0 : med_frame(4, 0x66, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0x37 && len >= 2 + MED_MSGID + MED_SKEY + MED_WORLDNAME) {
                                      /* FindWorldByName (pass 10, after 4/0x66; 108 bytes: MessageID, SessionKey,
                                       * name[64] = the party name, 2 pad, u32 world type 2) -> 1/0x38 (KZ3 coder
                                       * 0xA21240 = Horizon's model, 144 bytes): MessageID[21], 3 pad, i32 status,
                                       * i32 app id, AppName[32], i32 app type, i32 world id, WorldName[64], i32 world
                                       * status, u8 end-of-list, 3 pad. The game callback 0x154d24 keeps the world id
                                       * (+0x44) for the join on status 0. */
        char name[MED_WORLDNAME];
        memcpy(name, p + 2 + MED_MSGID + MED_SKEY, sizeof(name));   /* untrusted: bounded copy, forced NUL */
        name[sizeof(name) - 1] = 0;
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, RTCAP_APP_ID);
        medw_str(&w, "", MED_APPNAME);
        medw_u32(&w, 0);                     /* MediusAppTypeGame */
        medw_u32(&w, RTCAP_PARTY_WORLD);
        medw_str(&w, name, MED_WORLDNAME);
        medw_u32(&w, 1);                     /* WorldStaging. ASSUMPTION: the client does not read it */
        uint8_t eol[4] = { 1, 0, 0, 0 };
        medw_bytes(&w, eol, 4);
        return w.bad ? 0 : med_frame(1, 0x38, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x87 && len >= 2 + MED_MSGID + MED_SKEY + 2 + 4 + MED_GAMEPW + 4) {
                                      /* the party join (pass 10, after 1/0x38; KZ3 coder 0xA15B54: MessageID,
                                       * SessionKey, 2 pad, u32 world id, password[32], u32 game host type, RSA key,
                                       * 2 addresses, u32) -> 4/0x8E (registered right after it; coder 0xA15910, 196
                                       * bytes): MessageID[21], 3 pad, i32 status, i32 game host type,
                                       * NetConnectionInfo (156), 2 x u32 (ASSUMPTION: max players and one more; Horizon's
                                       * JoinGameResponse has one u32 there). The game callback 0x154e1c copies the
                                       * NetConnectionInfo on status 0 and connects to its first address
                                       * (0x1536a8 -> 0x9D9ECC). */
        uint32_t host_type = le32(p + 2 + MED_MSGID + MED_SKEY + 2 + 4 + MED_GAMEPW);
        if (host_type > 4) host_type = 0;    /* untrusted: only the five MGCL host types are echoed */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, host_type);
        med_net_connection_info_to(&w, RTCAP_DME_PORT, RTCAP_PARTY_WORLD);
        medw_u32(&w, 8);                     /* ASSUMPTION: max players ("Squad 0/8") */
        medw_u32(&w, 0);
        return w.bad ? 0 : med_frame(4, 0x8E, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x88) {   /* pass 10, sent when the player picks a match type in the party lobby (348
                                       * bytes; coder 0xA14F60; sender 0xA071BC from 0x15FE94; ASSUMPTION: a matchmaking
                                       * request) -> 4/0x89 (shared status coder 0xA152A4, 28 bytes): MessageID[21], 3
                                       * pad, i32 status. The game callback 0x16069c takes status 11 as accepted (state
                                       * 1 -> 2 with a timer); any other value is an error (1..10 -> 7000 + status). */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 11);
        size_t n = w.bad ? 0 : med_frame(4, 0x89, body, w.n, out, cap);
        if (!n) return 0;
        size_t b = med_match_found(p, len, body, sizeof(body));   /* pass 13: the 4/0x8A push right behind it */
        size_t m = b ? med_frame(4, 0x8A, body, b, out + n, cap - n) : 0;
        if (!b) printf("[kz3net-cap]   4/0x88 not parsed: no 4/0x8A push\n");
        return n + m;
    }
    if (cls == 4 && type == 0x35 && len >= 2 + MED_MSGID + MED_SKEY + 2 + 4) {
                                      /* pass 13 (section 16): GameInfo, sent by the game-join object 0x160FA8 right
                                       * after the 4/0x8A push (46 bytes: MessageID, SessionKey, 2 pad, u32 world id)
                                       * -> 4/0x36 GameInfoResponse (KZ3 coder 0xA1E5D0 = Horizon's model, 416 bytes):
                                       * MessageID[21], 3 pad, i32 status, app id, min, max, game level, skill, player
                                       * count, GameStats[256], GameName[64], rules set, 8 generic fields, world status,
                                       * game host type. The callback 0x162168 needs status 0 (state 3 -> 4), reads the
                                       * name and the stats text and keeps generic field 7. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, RTCAP_APP_ID);
        medw_u32(&w, 1);                     /* min players */
        medw_u32(&w, 24);                    /* max players. ASSUMPTION: the request's +0x14 (24) */
        medw_u32(&w, 0);
        medw_u32(&w, 0);
        medw_u32(&w, 1);                     /* player count */
        medw_bytes(&w, NULL, MED_STATS);     /* ASSUMPTION: empty stats text */
        medw_str(&w, "Loopback match", MED_WORLDNAME);
        for (int i = 0; i < 9; i++) medw_u32(&w, 0);   /* rules set, generic fields 1-8 */
        medw_u32(&w, 1);                     /* MediusWorldStatus WorldStaging (a match filling up) */
        medw_u32(&w, 0);                     /* MediusGameHostClientServer */
        return w.bad ? 0 : med_frame(4, 0x36, body, w.n, out, cap);
    }
    if (cls == 1 && type == 0xF3 && len >= 2 + MED_MSGID + MED_SKEY + 2 + 4 + 4 + MED_GAMEPW + 4) {
                                      /* pass 13: JoinGame (Horizon's model; KZ3 sends 202 bytes: MessageID, SessionKey,
                                       * 2 pad, u32 world id (the match, 3), join type, password[32], game host type,
                                       * RSA key, NetAddressList, u32), sent by 0x160C98 after 4/0x36 -> 1/0x24
                                       * JoinGameResponse (coder 0xA1574C, 192 bytes): MessageID[21], 3 pad, i32 status,
                                       * i32 game host type, NetConnectionInfo (156), u32 (Horizon's pad for v >= 112).
                                       * The callback 0x1622B0 needs status 0 (state 5 -> 6) and hands the
                                       * NetConnectionInfo to 0x1692D4 (the game connection). */
        uint32_t host_type = le32(p + 2 + MED_MSGID + MED_SKEY + 2 + 4 + 4 + MED_GAMEPW);
        if (host_type > 4) host_type = 0;    /* untrusted: only the five MGCL host types are echoed */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, host_type);
        med_net_connection_info_to(&w, RTCAP_DME_PORT, RTCAP_MATCH_WORLD);
        medw_u32(&w, 0);
        return w.bad ? 0 : med_frame(1, 0x24, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x2F) {   /* pass 13: CreateGame (sender 0xA08A24 from 0x16108C, when the 4/0x8A record's
                                       * world id is -1: this client hosts) -> 1/0x1E CreateGameResponse (coder 0xA1EC1C =
                                       * Horizon's model, 32 bytes): MessageID[21], 3 pad, i32 status, i32 world id. The
                                       * callback 0x1620B4 needs status 0 in state 1, hands the world id to the match
                                       * object (vfunc +0xB4) and goes to state 2 (JoinGame of that world). */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, RTCAP_MATCH_WORLD);
        return w.bad ? 0 : med_frame(1, 0x1E, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x2A) {   /* pass 13: GetServerTime (23 bytes: MessageID), sent by 0x16148C after 1/0x24
                                       * -> 4/0x2B (coder 0xA1DA08 = Horizon's model, 36 bytes): MessageID[21], 3 pad,
                                       * i32 status, u32 GMT time (Unix seconds), i32 time zone (MediusTimeZone_GMT 0).
                                       * The callback 0x162350 needs status 0 in state 7 and keeps both words (+0x58,
                                       * +0x5C), state 8. */
        MedW w = { body, 0, sizeof(body), 0 };
        medw_bytes(&w, msgid, MED_MSGID);
        medw_bytes(&w, NULL, 3);
        medw_u32(&w, 0);
        medw_u32(&w, (uint32_t)time(NULL));
        medw_u32(&w, 0);
        return w.bad ? 0 : med_frame(4, 0x2B, body, w.n, out, cap);
    }
    if (cls == 4 && type == 0x7B) {                                           /* SetLocalizationParams1 */
        size_t b = med_status(msgid, NULL, body, sizeof(body));
        return b ? med_frame(1, 0xA4, body, b, out, cap) : 0;
    }
    return 0;
}

/* The answer to a frame into out[0..cap): bytes to send (0 = none). */
static size_t rt_reply(const RtFrame* f, const uint8_t* hello, size_t hello_len, uint8_t* out, size_t cap)
{
    if (f->encrypted) return 0;
    if (f->id == 0x24 && hello && hello_len && hello_len <= 0xFFFF && cap >= 3 + hello_len) {
        out[0] = 0x25; out[1] = (uint8_t)hello_len; out[2] = (uint8_t)(hello_len >> 8);
        memcpy(out + 3, hello, hello_len);
        return 3 + hello_len;
    }
    if (f->id == 0x00 && cap >= 6) {
        static const uint8_t req[6] = { 0x22, 3, 0, 0, 0x48, 0x02 };   /* no password, max packet 584 */
        memcpy(out, req, sizeof(req));
        return sizeof(req);
    }
    /* CLIENT_CONNECT_READY_REQUIRE (0x23) -> SERVER_CONNECT_ACCEPT_TCP (0x07) as the server model answers a PS3 client
     * (no CRYPTKEY_GAME): u16 player id 0, u32 scert id 0x10EC, u16 player count 1, the client's address as a
     * 16-byte NUL-padded string. CLIENT_CONNECT_READY_TCP (0x21) -> SERVER_CONNECT_COMPLETE (0x1A) {u16 1}. */
    if (f->id == 0x23 && cap >= 3 + 24) {
        static const uint8_t acc[3 + 24] = { 0x07, 24, 0, 0, 0, 0xEC, 0x10, 0, 0, 1, 0,
                                             '1', '2', '7', '.', '0', '.', '0', '.', '1' };
        memcpy(out, acc, sizeof(acc));
        return sizeof(acc);
    }
    if (f->id == 0x21 && cap >= 5) {
        static const uint8_t done[5] = { 0x1A, 2, 0, 1, 0 };
        memcpy(out, done, sizeof(done));
        return sizeof(done);
    }
    /* CLIENT_TOKEN_MESSAGE (0x2F) {u8 type 2, u16 token} -> SERVER_TOKEN_MESSAGE (0x30) {u8 8, u16 token}: the token is
     * granted to the sender (pass 10, the party connection on the DME listener). KZ3's handler 0xA25C48 reads type 7
     * {token, owner}, 8 {token: owner = this client}, 9 {token: no owner}, 0xB..0xD (lists). ASSUMPTION: client type
     * 2 is the request (sent by 0x9E7CD8 when 0x9E7198 reports no owner); Horizon's DME ignores token messages. */
    if (f->id == 0x2F && f->len >= 3 && f->payload[0] == 2 && cap >= 6) {
        out[0] = 0x30; out[1] = 3; out[2] = 0; out[3] = 8; out[4] = f->payload[1]; out[5] = f->payload[2];
        return 6;
    }
    /* CLIENT_TIMEBASE_QUERY (0x2D) {u32 client time} -> SERVER_TIMEBASE_QUERY_NOTIFY (0x2E) {u32 client time, u32 server
     * time in ms} as Horizon's DME answers it (pass 13: KZ3 sends it on the game connection every few seconds). */
    if (f->id == 0x2D && f->len >= 4 && cap >= 11) {
        uint32_t ms = (uint32_t)((uint64_t)clock() * 1000u / CLOCKS_PER_SEC);
        out[0] = 0x2E; out[1] = 8; out[2] = 0;
        memcpy(out + 3, f->payload, 4);
        out[7] = (uint8_t)ms; out[8] = (uint8_t)(ms >> 8); out[9] = (uint8_t)(ms >> 16); out[10] = (uint8_t)(ms >> 24);
        return 11;
    }
    if (f->id == 0x0B) return med_reply(f->payload, f->len, out, cap);
    return 0;
}

static void rt_log_frame(uint16_t port, int conn, const RtFrame* f, size_t* logged)
{
    char line[1700], desc[300];
    size_t show = f->len < 96 ? f->len : 96;
    if (f->id == 0x0B && f->len >= 2 && f->payload[0] == 4 && f->payload[1] == 0x88)
        show = f->len < 500 ? f->len : 500;           /* pass 13: the whole matchmaking request (no secrets in it) */
    if (*logged >= RTCAP_LOG_MAX) show = 0;
    if (f->id == 0x00 && show > 0x48) show = 0x48;   /* never the token bytes */
    if (f->id == 0x3B) show = 0;                      /* a container: its parts are logged one by one */
    if (f->id == 0x0B && f->len >= 2 && f->payload[0] == 4 && f->payload[1] == 0x0A && show > 2 + MED_MSGID + MED_SKEY)
        show = 2 + MED_MSGID + MED_SKEY;              /* DnasSignaturePost: never the signature bytes */
    if (f->id == 0x0B && f->len >= 2 && f->payload[0] == 4 && f->payload[1] == 0x58) {
        size_t at = rt_ticket_at(f->payload, f->len); /* TicketLogin: the bytes before the ticket, never the ticket */
        size_t lim = at ? at : 2 + MED_MSGID + MED_SKEY;
        if (show > lim) show = lim;
    }
    int w = snprintf(line, sizeof(line), "[kz3net-cap] %u conn %d recv RT 0x%02X%s len %u:", (unsigned)port, conn, f->id,
                     f->encrypted ? " (enc)" : "", (unsigned)f->len);
    for (size_t i = 0; i < show && w > 0 && (size_t)w < sizeof(line) - 4; i++)
        w += snprintf(line + w, sizeof(line) - (size_t)w, " %02x", f->payload[i]);
    *logged += show;
    printf("%s\n", line);
    rt_describe(f, desc, sizeof(desc));
    if (desc[0]) printf("[kz3net-cap] %u conn %d   %s\n", (unsigned)port, conn, desc);
    fflush(stdout);
}

/* The SVO region list (pass 7, the RE notes section 12): KZ3 registers the GUM data source "urn:regionlist" as
 * an HTTP GET of the universe's SVO URL + "kin/stats/region_list.jsp" (v1.00 @0x89122c) and reads it with the XPaths
 * "region/" (list), "region[i]/id/text()" (menu label menu_region_<id>, the stat region) and
 * "region[i]/mediuslocation/text()" (the Medius location id passed to SelectRegionAndLocation). One region on our
 * location 1. KZ3_NET_CAPTURE_REGIONS = a file whose bytes replace this document (test variants, <= 4 KiB). */
#define RTCAP_REGIONS_PATH "kin/stats/region_list.jsp"
#define RTCAP_REGIONS_DOC  "<data><region><id>1</id><mediuslocation>1</mediuslocation></region></data>\n"
#define RTCAP_REGIONS_MAX  4096u

/* The SVO login (pass 9, the RE notes section 14.1): POST SVO URL + "kin/account/nplogin.do" (body = the NP
 * ticket). The answer is read by v1.00 @0x8923a8 through a plain XML DOM (node factory @0x116728 takes any element
 * name): root element (any name) -> child "data" -> child "loginresult", text read with strtol; 30000 = logged in
 * (state 12), anything else = error 20001; no root = error 20002. Optional children of data, all absent here:
 * urnstore (urn/name, urn/uri: data sources), personalstats, friendsboard, clan. ASSUMPTION: the root's name (not read
 * by the client). KZ3_NET_CAPTURE_LOGIN = a file whose bytes replace this document (test variants, <= 4 KiB). */
#define RTCAP_LOGIN_PATH "kin/account/nplogin.do"
#define RTCAP_LOGIN_DOC  "<svo><data><loginresult>30000</loginresult></data></svo>\n"

typedef struct {
    uint8_t    hello[RTCAP_HELLO_MAX];
    size_t     hello_len;
    char       regions[RTCAP_REGIONS_MAX + 1];
    size_t     regions_len;
    char       login[RTCAP_REGIONS_MAX + 1];
    size_t     login_len;
    char       label_xml[KZ3_NET_LABEL_MAX_BYTES * 6 + 1];   /* the region label, XML-escaped ("" = none) */
} RtCap;

static RtCap s_cap;

typedef struct {
    rtcap_sock lsock;
    uint16_t   port;
    int        http;      /* 1 = the SVO HTTP logger */
} RtListener;

static RtListener s_listen[6];
static int        s_nlisten;

/* One frame: logged, answered when rt_reply knows it. RT_MSG_CLIENT_MULTI_APP_TOSERVER (0x3B, pass 7: KZ3 sends
 * DnasSignaturePost 4/0x0A and TicketLogin 4/0x58 together in one) is a sequence of whole sub-frames (u8 id, u16 LE
 * length, payload): each is handled like a frame of its own, never past the container's payload. */
static void rt_handle(rtcap_sock c, uint16_t port, int conn, const RtFrame* f, uint8_t* out, size_t* logged, int depth)
{
    rt_log_frame(port, conn, f, logged);
    static int s_peeks;
    if (f->id == 0x05 && port == RTCAP_MLS_PORT && s_peeks < 6) { s_peeks++; rt_peek_matchmaker(); }   /* test only */
    if (f->id == 0x3B && depth == 0) {
        RtFrame sf;
        size_t off = 0, used, k = 0;
        while (off < f->len && (used = rt_frame_parse(f->payload + off, f->len - off, &sf)) > 0) {
            printf("[kz3net-cap] %u conn %d   0x3B part %u:\n", (unsigned)port, conn, (unsigned)++k);
            rt_handle(c, port, conn, &sf, out, logged, 1);
            off += used;
        }
        if (off != f->len) printf("[kz3net-cap] %u conn %d   0x3B: %u trailing bytes not a whole frame\n", (unsigned)port,
                                  conn, (unsigned)(f->len - off));
        return;
    }
    size_t n = rt_reply(f, s_cap.hello, s_cap.hello_len, out, RTCAP_OUT);
    if (n) {
        int sent = (int)send(c, (const char*)out, (int)n, 0);
        for (size_t o = 0; o + 3 <= n;) {   /* one line per frame sent (several for a universe answer) */
            size_t fl = 3u + (size_t)(out[o + 1] | out[o + 2] << 8);
            if (out[o] == 0x0A && o + 5 <= n)
                printf("[kz3net-cap] %u conn %d sent RT 0x0A len %u: medius class %u type 0x%02X (%d)\n",
                       (unsigned)port, conn, (unsigned)(fl - 3), out[o + 3], out[o + 4], sent);
            else
                printf("[kz3net-cap] %u conn %d sent RT 0x%02X len %u (%d)\n", (unsigned)port, conn, out[o],
                       (unsigned)(fl - 3), sent);
            o += fl;
        }
    } else if (f->id == 0x24) {
        printf("[kz3net-cap] conn %d   no KZ3_NET_CAPTURE_HELLO file: hello left unanswered\n", conn);
    } else if (f->id == 0x0B && f->len >= 2 && f->payload[0] == 4 && f->payload[1] == 0x58) {
        printf("[kz3net-cap] conn %d   TicketLogin reached: left unanswered (no account server here)\n", conn);
    }
    fflush(stdout);
}

static void rt_serve(rtcap_sock c, uint16_t port, int conn, uint8_t* buf, uint8_t* out)
{
    size_t have = 0, logged = 0;
    for (;;) {
        int r = (int)recv(c, (char*)buf + have, (int)(RTCAP_BUF - have), 0);
        if (r <= 0) { printf("[kz3net-cap] %u conn %d closed by the game (%d)\n", (unsigned)port, conn, r); break; }
        have += (size_t)r;
        RtFrame f;
        size_t used;
        while ((used = rt_frame_parse(buf, have, &f)) > 0) {
            rt_handle(c, port, conn, &f, out, &logged, 0);
            memmove(buf, buf + used, have - used);
            have -= used;
        }
        if (have == RTCAP_BUF) { printf("[kz3net-cap] conn %d: frame larger than the buffer, closing\n", conn); break; }
    }
    rtcap_close(c);
}

void kz3_net_capture_set_region_label(const char* label)
{
    s_cap.label_xml[0] = 0;
    if (label && *label && kz3_net_valid_region_label(label))
        kz3_net_xml_escape(label, s_cap.label_xml, sizeof(s_cap.label_xml));
}

/* 1 when the request line is METHOD (one of the '|'-separated names) and its path, query cut, ends in suffix. */
static int http_route(const char* line, const char* methods, const char* suffix)
{
    const char* sp = strchr(line, ' ');
    if (!sp) return 0;
    size_t ml = (size_t)(sp - line);
    int mok = 0;
    for (const char* m = methods; *m;) {
        const char* bar = strchr(m, '|');
        size_t l = bar ? (size_t)(bar - m) : strlen(m);
        if (l == ml && !memcmp(m, line, l)) mok = 1;
        m += l + (bar ? 1 : 0);
    }
    if (!mok) return 0;
    const char* path = sp + 1;
    const char* end = strchr(path, ' ');
    const char* q = strchr(path, '?');
    if (!end) end = path + strlen(path);
    if (q && q < end) end = q;
    size_t pl = (size_t)(end - path), tl = strlen(suffix);
    return pl >= tl && !memcmp(end - tl, suffix, tl);
}

/* The built-in region list: one region on location 1; with a region label (online.ini region =, pass 9) the region
 * also carries <name>LABEL</name> (escaped), which the stock region page does not read (its label is the
 * localisation key menu_region_<id>; the port's Localizer hook shows the label, kz3_net_hooks.h). */
static size_t http_regions_doc(char* out, size_t cap)
{
    int n = s_cap.label_xml[0]
          ? snprintf(out, cap, "<data><region><id>1</id><mediuslocation>1</mediuslocation><name>%s</name></region></data>\n",
                     s_cap.label_xml)
          : snprintf(out, cap, "%s", RTCAP_REGIONS_DOC);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* The answer to one request line: the region list for a GET whose path ends in RTCAP_REGIONS_PATH, the login document
 * for a POST to RTCAP_LOGIN_PATH (queries ignored), 404 with no body otherwise. Returns the bytes written to out, 0
 * when out is too small. */
static size_t http_answer(const char* line, uint8_t* out, size_t cap)
{
    char built[RTCAP_REGIONS_MAX + 1];
    const char* doc = NULL;
    size_t dlen = 0;
    if (http_route(line, "GET", RTCAP_REGIONS_PATH)) {
        if (s_cap.regions_len) { doc = s_cap.regions; dlen = s_cap.regions_len; }
        else { dlen = http_regions_doc(built, sizeof(built)); doc = dlen ? built : NULL; }
    } else if (http_route(line, "POST|GET", RTCAP_LOGIN_PATH)) {
        doc = s_cap.login_len ? s_cap.login : RTCAP_LOGIN_DOC;
        dlen = s_cap.login_len ? s_cap.login_len : sizeof(RTCAP_LOGIN_DOC) - 1;
    }
    char head[160];
    int hl = doc ? snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %u\r\n"
                            "Connection: close\r\n\r\n", (unsigned)dlen)
                 : snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    if (!doc) dlen = 0;
    if (hl <= 0 || (size_t)hl + dlen > cap) return 0;
    memcpy(out, head, (size_t)hl);
    if (dlen) memcpy(out + hl, doc, dlen);
    return (size_t)hl + dlen;
}

/* The HTTP logger (SVO, port 10070): one request per connection, logged as its request line and header names
 * (values only for Host, Content-Length and Content-Type; never cookies or other values), the body as its length;
 * answered by http_answer. Returns the bytes of the answer written to out (0 = not a complete request yet). */
/* The request line as logged (pass 8): query values dropped, names kept ("?sig=&sigclass=..."); nplogin.do carries
 * sig = the open PSID. Always NUL-terminates out (cap >= 1). */
static void http_redact_query(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    int in_q = 0, in_v = 0;
    for (size_t k = 0; in[k] && o + 1 < cap; k++) {
        char ch = in[k];
        if (ch == ' ') in_q = in_v = 0;
        else if (ch == '?' && !in_q) in_q = 1;
        else if (in_q && ch == '&') in_v = 0;
        else if (in_q && ch == '=') { out[o++] = '='; in_v = 1; continue; }
        if (!in_v) out[o++] = ch;
    }
    out[o] = 0;
}

static size_t http_log_and_reply(uint16_t port, int conn, const uint8_t* buf, size_t n, uint8_t* out, size_t cap)
{
    char first[300] = "";
    size_t hend = 0;
    for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(buf + i, "\r\n\r\n", 4)) { hend = i + 4; break; }
    if (!hend) return 0;
    size_t clen = 0;
    char line[300];
    size_t ls = 0;
    for (size_t i = 0; i + 1 < hend; i++) {
        if (buf[i] != '\r' || buf[i + 1] != '\n') continue;
        size_t ll = i - ls;
        if (ll) {
            size_t show = ll < sizeof(line) - 1 ? ll : sizeof(line) - 1;
            for (size_t k = 0; k < show; k++) line[k] = (buf[ls + k] >= 0x20 && buf[ls + k] < 0x7F) ? (char)buf[ls + k] : '.';
            line[show] = 0;
            if (ls == 0) {
                char shown[300];
                http_redact_query(line, shown, sizeof(shown));
                printf("[kz3net-cap] %u conn %d HTTP %s\n", (unsigned)port, conn, shown);
                memcpy(first, line, show + 1);
            }
            else {
                char* colon = strchr(line, ':');
                int keep = colon && (!strncasecmp(line, "Host:", 5) || !strncasecmp(line, "Content-Length:", 15) ||
                                     !strncasecmp(line, "Content-Type:", 13) || !strncasecmp(line, "User-Agent:", 11));
                if (colon && !strncasecmp(line, "Content-Length:", 15)) clen = (size_t)strtoul(colon + 1, NULL, 10);
                if (colon && !keep) colon[1] = 0;
                printf("[kz3net-cap] %u conn %d   %s%s\n", (unsigned)port, conn, line, colon && !keep ? " (value not logged)" : "");
            }
        }
        ls = i + 2;
    }
    if (clen && n - hend < clen && n < RTCAP_BUF) return 0;    /* wait for the body */
    printf("[kz3net-cap] %u conn %d   body %u bytes (not logged)\n", (unsigned)port, conn, (unsigned)(n - hend));
    return http_answer(first, out, cap);
}

static void http_serve(rtcap_sock c, uint16_t port, int conn, uint8_t* buf, uint8_t* out)
{
    size_t have = 0;
    for (;;) {
        int r = (int)recv(c, (char*)buf + have, (int)(RTCAP_BUF - have), 0);
        if (r <= 0) { printf("[kz3net-cap] %u conn %d closed by the game (%d)\n", (unsigned)port, conn, r); break; }
        have += (size_t)r;
        size_t n = http_log_and_reply(port, conn, buf, have, out, RTCAP_OUT);
        if (n) {
            int sent = (int)send(c, (const char*)out, (int)n, 0);
            printf("[kz3net-cap] %u conn %d sent HTTP %.3s, %u bytes (%d)\n", (unsigned)port, conn, (const char*)out + 9,
                   (unsigned)n, sent);
            break;
        }
        if (have == RTCAP_BUF) { printf("[kz3net-cap] %u conn %d: request larger than the buffer\n", (unsigned)port, conn); break; }
    }
    fflush(stdout);
    rtcap_close(c);
}

/* One thread per accepted connection (pass 8): the game keeps its MLS connection open while it may open
 * another (a channel join can point at a new address), so a listener never blocks on one peer. */
typedef struct { RtListener* l; rtcap_sock c; int conn; } RtConnArg;

#ifdef _WIN32
static DWORD WINAPI rt_conn_thread(LPVOID arg)
#else
static void* rt_conn_thread(void* arg)
#endif
{
    RtConnArg* a = (RtConnArg*)arg;
    uint8_t* buf = (uint8_t*)malloc(RTCAP_BUF);
    uint8_t* out = (uint8_t*)malloc(RTCAP_OUT);
    if (buf && out) {
        if (a->l->http) http_serve(a->c, a->l->port, a->conn, buf, out);
        else rt_serve(a->c, a->l->port, a->conn, buf, out);
    } else rtcap_close(a->c);
    free(buf);
    free(out);
    free(a);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI rt_cap_thread(LPVOID arg)
#else
static void* rt_cap_thread(void* arg)
#endif
{
    RtListener* l = (RtListener*)arg;
    for (int conn = 1; conn <= (l->http ? 64 : 8); conn++) {
        rtcap_sock c = accept(l->lsock, NULL, NULL);
        if (c == RTCAP_BAD) break;
        printf("[kz3net-cap] %u conn %d accepted\n", (unsigned)l->port, conn);
        fflush(stdout);
        RtConnArg* a = (RtConnArg*)malloc(sizeof(*a));
        if (!a) { rtcap_close(c); continue; }
        a->l = l; a->c = c; a->conn = conn;
#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, rt_conn_thread, a, 0, NULL);
        if (t) CloseHandle(t); else rt_conn_thread(a);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, rt_conn_thread, a) == 0) pthread_detach(t); else rt_conn_thread(a);
#endif
    }
    rtcap_close(l->lsock);
    return 0;
}

/* One listener on 127.0.0.1:port (MUIS 10071, MAS 10075 in the game; any port in the host test). The hello payload
 * is read at the first call; online_id (may be NULL) only feeds the TicketLogin "matches" check. */
int kz3_net_capture_start(uint16_t port, const char* hello_file, const char* online_id)
{
    return kz3_net_capture_start_kind(port, hello_file, online_id, 0);
}

int kz3_net_capture_start_kind(uint16_t port, const char* hello_file, const char* online_id, int http)
{
    struct sockaddr_in a;
    static int loaded;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (s_nlisten >= (int)(sizeof(s_listen) / sizeof(s_listen[0]))) return -1;
    if (online_id) snprintf(s_cap_online_id, sizeof(s_cap_online_id), "%s", online_id);
    if (!loaded) {
        loaded = 1;
        s_cap.hello_len = 0;
        if (hello_file && *hello_file) {
            FILE* f = fopen_utf8(hello_file);
            if (f) {
                s_cap.hello_len = fread(s_cap.hello, 1, sizeof(s_cap.hello), f);
                if (fgetc(f) != EOF) s_cap.hello_len = 0;   /* larger than RTCAP_HELLO_MAX: refused */
                fclose(f);
            }
            printf("[kz3net-cap] server hello payload: %u bytes%s\n", (unsigned)s_cap.hello_len,
                   s_cap.hello_len ? "" : " (file missing, empty or too large: hello unanswered)");
        }
        const char* rf = getenv("KZ3_NET_CAPTURE_REGIONS");   /* test variants of the region list */
        s_cap.regions_len = 0;
        if (rf && *rf) {
            FILE* f = fopen_utf8(rf);
            if (f) {
                s_cap.regions_len = fread(s_cap.regions, 1, RTCAP_REGIONS_MAX, f);
                if (fgetc(f) != EOF) s_cap.regions_len = 0;    /* larger than RTCAP_REGIONS_MAX: built-in document */
                fclose(f);
            }
            printf("[kz3net-cap] region list file: %u bytes%s\n", (unsigned)s_cap.regions_len,
                   s_cap.regions_len ? "" : " (missing, empty or too large: built-in document)");
        }
        const char* lf = getenv("KZ3_NET_CAPTURE_LOGIN");     /* test variants of the nplogin.do answer */
        s_cap.login_len = 0;
        if (lf && *lf) {
            FILE* f = fopen_utf8(lf);
            if (f) {
                s_cap.login_len = fread(s_cap.login, 1, RTCAP_REGIONS_MAX, f);
                if (fgetc(f) != EOF) s_cap.login_len = 0;      /* larger than 4 KiB: built-in document */
                fclose(f);
            }
            printf("[kz3net-cap] login answer file: %u bytes%s\n", (unsigned)s_cap.login_len,
                   s_cap.login_len ? "" : " (missing, empty or too large: built-in document)");
        }
    }
    RtListener* l = &s_listen[s_nlisten];
    l->port = port;
    l->http = http;
    l->lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (l->lsock == RTCAP_BAD) { log_line("capture: no socket"); return -1; }
    int one = 1;
    setsockopt(l->lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 only */
    if (bind(l->lsock, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(l->lsock, 4) != 0) {
        log_line("capture: cannot listen on 127.0.0.1");
        rtcap_close(l->lsock);
        return -1;
    }
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, rt_cap_thread, l, 0, NULL);
    if (!t) { rtcap_close(l->lsock); return -1; }
    CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, rt_cap_thread, l) != 0) { rtcap_close(l->lsock); return -1; }
    pthread_detach(t);
#endif
    s_nlisten++;
    printf("[kz3net-cap] listening on 127.0.0.1:%u (test capture%s)\n", (unsigned)port, http ? ", HTTP" : "");
    fflush(stdout);
    return 0;
}
