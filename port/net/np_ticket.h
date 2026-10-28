/*
 * KZ3 port: an unsigned NP ticket, version 2.1, as sceNpManagerGetTicket hands it to the title.
 *
 * The title forwards the ticket unread to its Medius server (TicketLogin, 4/0x58). A self-hosted server (Horizon,
 * MAS.cs:754-845) reads only the 32-byte online id at a fixed offset and trusts it; nobody checks the signature, so
 * the footer carries zeros. Layout (big-endian, every entry = u16 type, u16 length, data):
 *
 *   0x00 u32 version 0x21010000          0x04 u32 size of everything after this field
 *   0x08 section 0x3000 (body), u16 length
 *   0x0C serial      binary  20          0x24 issuer id  u32
 *   0x2C issued      time    8 (ms)      0x38 expires    time 8 (ms)
 *   0x44 user id     u64                 0x50 online id  string 32   (data at 0x54 = 84)
 *   0x74 region      binary  4           0x7C domain     string 4
 *   0x84 service id  binary  24          0xA0 status     u32
 *   0xA8 empty, 0xAC empty
 *   0xB0 section 0x3002 (footer): cipher id binary 4, signature binary 56 (zeros)
 *
 * Entry types: 0 empty, 1 u32, 2 u64, 4 string, 7 time, 8 binary. ASSUMPTION: the two empty entries and the footer
 * sizes follow tickets seen on PS3 v2.1 (not recovered from KZ3; KZ3 never parses the ticket).
 */
#ifndef KZ3_NP_TICKET_H
#define KZ3_NP_TICKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_TICKET_V21_SIZE       0xF8u   /* whole ticket, bytes */
#define NP_TICKET_ONLINE_ID_OFF  0x54u   /* the 32-byte online id field */
#define NP_TICKET_SERVICE_ID_OFF 0x88u

typedef struct {
    char     online_id[17];      /* 3-16 chars, validated by the caller (np_ticket_valid_online_id) */
    char     service_id[25];     /* from sceNpManagerRequestTicket2; at most 24 bytes are used */
    uint64_t issued_ms;          /* ms since 1970 */
    uint64_t expire_ms;
    uint64_t user_id;            /* 0 = derived from the online id */
    uint32_t issuer_id;
    uint32_t status;
    char     region[4];          /* e.g. "us" */
    char     domain[4];          /* e.g. "un" */
} NpTicketFields;

/* PSN rules: 3-16 of [A-Za-z0-9_-], starting with a letter. 1 if valid. */
int np_ticket_valid_online_id(const char* id);

/* A stable 64-bit user id from the online id (FNV-1a), never 0. */
uint64_t np_ticket_user_id(const char* online_id);

/* Writes the ticket into out[0..cap). Returns NP_TICKET_V21_SIZE, or 0 if cap is too small or the online id is
 * invalid (nothing written then). */
size_t np_ticket_build(const NpTicketFields* f, uint8_t* out, size_t cap);

/* ---- a ticket the player provides (online.ini ticket_file) -------------------------------------------------
 * The bytes are untrusted. np_ticket_check accepts only the shape of a PSN ticket as publicly documented
 * (the RE notes section 9.2): u32 version (major 2..4 in the high nibble of byte 0, bytes 2..3 zero), u32 size =
 * the rest of the file exactly, then sections (u16 type 0x30xx, u16 length) of typed entries (0 empty, 1 u32,
 * 2 u64, 4 string, 7 time, 8 binary; fixed lengths enforced; sections nest at most two deep), every length inside
 * its parent, nothing left over. The body (first section, 0x3000) must start with the v2.x order: serial (binary),
 * issuer (u32), issued (time), expires (time), user id (u64), online id (string), region (binary), domain (string),
 * service id (binary); the online id must be a valid PSN id. Nothing in it is ever logged except the version, size,
 * online id, service id and times. */
#define NP_TICKET_FILE_MAX 2048u

typedef struct {
    uint32_t version;            /* e.g. 0x21010000 */
    uint32_t size;               /* whole ticket, bytes */
    char     online_id[17];
    char     service_id[25];     /* printable bytes up to the first NUL; "" if none */
    uint64_t issued_ms, expire_ms;
    int      body_entries;
} NpTicketInfo;

enum {
    NP_TICKET_OK = 0, NP_TICKET_E_SIZE = -1, NP_TICKET_E_VERSION = -2, NP_TICKET_E_LENGTH = -3,
    NP_TICKET_E_ENTRY = -4, NP_TICKET_E_LAYOUT = -5, NP_TICKET_E_ONLINE_ID = -6, NP_TICKET_E_TRAILING = -7
};

/* 0 if buf[0..len) has a ticket's shape (info filled when not NULL), else one of the negative reasons above. */
int np_ticket_check(const uint8_t* buf, size_t len, NpTicketInfo* info);
const char* np_ticket_reason(int rc);

#ifdef __cplusplus
}
#endif

#endif /* KZ3_NP_TICKET_H */
