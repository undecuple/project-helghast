/*
 * KZ3 port: unsigned NP ticket v2.1 builder (see np_ticket.h).
 */
#include "np_ticket.h"

#include <ctype.h>
#include <string.h>

int np_ticket_valid_online_id(const char* id)
{
    size_t n = 0;
    if (!id || !isalpha((unsigned char)id[0])) return 0;
    for (; id[n]; n++) {
        unsigned char c = (unsigned char)id[n];
        if (n >= 16 || !(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return n >= 3;
}

uint64_t np_ticket_user_id(const char* online_id)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char* p = (const unsigned char*)online_id; p && *p; p++) {
        h ^= *p;
        h *= 0x100000001B3ull;
    }
    return h ? h : 1;
}

typedef struct { uint8_t* p; size_t n; } W;

static void be16(W* w, uint16_t v) { w->p[w->n++] = (uint8_t)(v >> 8); w->p[w->n++] = (uint8_t)v; }
static void be32(W* w, uint32_t v) { be16(w, (uint16_t)(v >> 16)); be16(w, (uint16_t)v); }
static void be64(W* w, uint64_t v) { be32(w, (uint32_t)(v >> 32)); be32(w, (uint32_t)v); }
static void head(W* w, uint16_t type, uint16_t len) { be16(w, type); be16(w, len); }
/* `len` bytes: the string's bytes (at most len), zero padded. */
static void bytes(W* w, const char* s, size_t len)
{
    size_t k = s ? strnlen(s, len) : 0;
    memcpy(w->p + w->n, s ? s : "", k);
    memset(w->p + w->n + k, 0, len - k);
    w->n += len;
}

size_t np_ticket_build(const NpTicketFields* f, uint8_t* out, size_t cap)
{
    if (!f || !out || cap < NP_TICKET_V21_SIZE || !np_ticket_valid_online_id(f->online_id)) return 0;
    W w = { out, 0 };
    be32(&w, 0x21010000u);
    be32(&w, NP_TICKET_V21_SIZE - 8);
    head(&w, 0x3000, 0xA4);                   /* body: 0x0C..0xB0 */
    /* serial: "KZ3PC" + the user id, so two tickets for one name share it (no secret in it) */
    uint64_t uid = f->user_id ? f->user_id : np_ticket_user_id(f->online_id);
    head(&w, 8, 20);
    bytes(&w, "KZ3PC", 12);
    be64(&w, uid);
    head(&w, 1, 4);  be32(&w, f->issuer_id);
    head(&w, 7, 8);  be64(&w, f->issued_ms);
    head(&w, 7, 8);  be64(&w, f->expire_ms);
    head(&w, 2, 8);  be64(&w, uid);
    head(&w, 4, 32); bytes(&w, f->online_id, 32);
    head(&w, 8, 4);  bytes(&w, f->region, 4);
    head(&w, 4, 4);  bytes(&w, f->domain, 4);
    head(&w, 8, 24); bytes(&w, f->service_id, 24);
    head(&w, 1, 4);  be32(&w, f->status);
    head(&w, 0, 0);
    head(&w, 0, 0);
    head(&w, 0x3002, 0x44);                   /* footer: 0xB4..0xF8 */
    head(&w, 8, 4);  bytes(&w, NULL, 4);
    head(&w, 8, 56); bytes(&w, NULL, 56);
    return w.n == NP_TICKET_V21_SIZE ? w.n : 0;
}

/* ---- shape check of a ticket the player provides --------------------------------------------------------- */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* Walks the entries of [p, p+n). depth 1 = inside a top-level section. Records the body's first entries. */
typedef struct { uint16_t type, len; const uint8_t* data; } Ent;

static int walk(const uint8_t* p, size_t n, int depth, Ent* first, int first_cap, int* count)
{
    size_t off = 0;
    while (off < n) {
        if (n - off < 4) return NP_TICKET_E_LENGTH;
        uint16_t type = rd16(p + off), len = rd16(p + off + 2);
        off += 4;
        if (len > n - off) return NP_TICKET_E_LENGTH;
        if ((type & 0xFF00u) == 0x3000u) {
            if (depth >= 2) return NP_TICKET_E_ENTRY;
            int sub = 0;
            int rc = walk(p + off, len, depth + 1, NULL, 0, &sub);
            if (rc) return rc;
        } else {
            switch (type) {
            case 0: if (len != 0) return NP_TICKET_E_ENTRY; break;
            case 1: if (len != 4) return NP_TICKET_E_ENTRY; break;
            case 2: case 7: if (len != 8) return NP_TICKET_E_ENTRY; break;
            case 4: case 8: break;
            default: return NP_TICKET_E_ENTRY;
            }
        }
        if (first && *count < first_cap) {
            first[*count].type = type; first[*count].len = len; first[*count].data = p + off;
        }
        (*count)++;
        off += len;
    }
    return 0;
}

int np_ticket_check(const uint8_t* buf, size_t len, NpTicketInfo* info)
{
    static const uint16_t want[9] = { 8, 1, 7, 7, 2, 4, 8, 4, 8 };
    NpTicketInfo t;
    Ent body[9];
    int nbody = 0;
    memset(&t, 0, sizeof(t));
    if (!buf || len < 12 || len > NP_TICKET_FILE_MAX) return NP_TICKET_E_SIZE;
    t.version = rd32(buf);
    unsigned major = buf[0] >> 4;
    if (major < 2 || major > 4 || buf[2] || buf[3]) return NP_TICKET_E_VERSION;
    t.size = (uint32_t)len;
    if (rd32(buf + 4) != len - 8) return NP_TICKET_E_TRAILING;
    /* top level: sections only; the first one is the body */
    size_t off = 8;
    int top = 0;
    while (off < len) {
        if (len - off < 4) return NP_TICKET_E_LENGTH;
        uint16_t type = rd16(buf + off), slen = rd16(buf + off + 2);
        off += 4;
        if ((type & 0xFF00u) != 0x3000u) return NP_TICKET_E_LAYOUT;
        if (slen > len - off) return NP_TICKET_E_LENGTH;
        int count = 0;
        int rc = walk(buf + off, slen, 1, top == 0 ? body : NULL, top == 0 ? 9 : 0, &count);
        if (rc) return rc;
        if (top == 0) {
            if (type != 0x3000u) return NP_TICKET_E_LAYOUT;
            nbody = count;
        }
        off += slen;
        top++;
    }
    if (nbody < 9) return NP_TICKET_E_LAYOUT;
    for (int i = 0; i < 9; i++)
        if (body[i].type != want[i]) return NP_TICKET_E_LAYOUT;
    t.body_entries = nbody;
    t.issued_ms = rd64(body[2].data);
    t.expire_ms = rd64(body[3].data);
    /* online id: NUL-padded string, at most 16 meaningful bytes */
    size_t k = 0;
    while (k < body[5].len && body[5].data[k]) {
        if (k >= 16) return NP_TICKET_E_ONLINE_ID;
        t.online_id[k] = (char)body[5].data[k];
        k++;
    }
    for (size_t j = k; j < body[5].len; j++)
        if (body[5].data[j]) return NP_TICKET_E_ONLINE_ID;   /* bytes after the NUL must be padding */
    if (!np_ticket_valid_online_id(t.online_id)) return NP_TICKET_E_ONLINE_ID;
    for (size_t j = 0; j < body[8].len && j < 24; j++) {
        uint8_t c = body[8].data[j];
        if (!c) break;
        t.service_id[j] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    if (info) *info = t;
    return 0;
}

const char* np_ticket_reason(int rc)
{
    switch (rc) {
    case NP_TICKET_OK:          return "ok";
    case NP_TICKET_E_SIZE:      return "file size out of range (12..2048 bytes)";
    case NP_TICKET_E_VERSION:   return "not a ticket version 2.x-4.x";
    case NP_TICKET_E_LENGTH:    return "an entry or section runs past its parent";
    case NP_TICKET_E_ENTRY:     return "unknown entry type or wrong fixed length";
    case NP_TICKET_E_LAYOUT:    return "body layout is not serial/issuer/issued/expires/user/online id/region/domain/service";
    case NP_TICKET_E_ONLINE_ID: return "online id field is not a valid PSN online id";
    case NP_TICKET_E_TRAILING:  return "size field does not match the file length";
    default:                    return "invalid";
    }
}
