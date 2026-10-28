/*
 * KZ3 port: online setup. Everything here is inert unless the player turned online on (KZ3_ONLINE, read once by
 * libs/network/net_policy.c, patch 4001). Online, the port:
 *   - reads the server from online.ini in the player's data folder (next to keybinds.ini),
 *   - maps KZ3's MUIS host name to that server and allows only its ports (patch 4001's strict egress policy:
 *     every other destination, port 53 and the telemetry host are refused),
 *   - signs the player in to NP with the configured online id and answers the ticket calls (np_kz3.c).
 * Every byte of the file is untrusted: bounded, validated, unknown keys ignored with a warning.
 */
#ifndef KZ3_NET_H
#define KZ3_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KZ3_NET_MUIS_HOST      "killzone3ps3.online.scee.com"          /* rt_socket gethostbyname @0x9F200C */
#define KZ3_NET_TELEMETRY_HOST "killzone3ps3telemetry.online.scee.com" /* cellHttp POSTs, 0x82040..0x84560 */
#define KZ3_NET_MAX_RANGES     8
#define KZ3_NET_CONFIG_MAX     (64 * 1024)

#define KZ3_NET_MAX_SERVERS    8
#define KZ3_NET_NAME_MAX       32               /* [server NAME]: bytes */
/* The region label shown in the game's region list. ASSUMPTION: 24 characters fit the
 * list row (the stock labels are short upper-case words such as "EUROPE"); BMP code points only (the game's text
 * strings are 16-bit, v1.00 @0x22684 decodes UTF-8 into them), so at most 3 UTF-8 bytes each. */
#define KZ3_NET_LABEL_MAX_CHARS 24
#define KZ3_NET_LABEL_MAX_BYTES (KZ3_NET_LABEL_MAX_CHARS * 3)

typedef struct {
    char     name[KZ3_NET_NAME_MAX + 1];
    char     host[254];
    uint16_t ranges[KZ3_NET_MAX_RANGES][2];
    int      nranges;
    char     region[KZ3_NET_LABEL_MAX_BYTES + 1];   /* UTF-8, validated; "" = the game's own label */
} Kz3NetServer;

typedef struct {
    /* The server in use (the top-level keys, or the [server NAME] section kz3_net_config_select copied here). */
    char     host[254];                       /* server: DNS name or dotted IPv4; "" = none configured */
    uint16_t ranges[KZ3_NET_MAX_RANGES][2];   /* allowed ports on the server (TCP and UDP) */
    int      nranges;
    char     region[KZ3_NET_LABEL_MAX_BYTES + 1];   /* region label of the server in use; "" = the game's own */
    char     online_id[17];                   /* "" = none */
    char     ticket_file[260];                /* "" = built-in unsigned ticket; else a file the player gives
                                                 (absolute, or relative to online.ini's folder) */
    char     server[KZ3_NET_NAME_MAX + 1];    /* top-level "server = NAME": which section to use; "" = the first */
    Kz3NetServer servers[KZ3_NET_MAX_SERVERS];   /* the [server NAME] sections, in file order */
    int      nservers;
} Kz3NetConfig;

typedef void (*kz3_net_log_fn)(const char* msg);

/* Defaults: no host, the ports Horizon listens on by default, no online id. ASSUMPTION: 10070-10080 (MUIS 10071 is
 * hard-wired in KZ3; Horizon's defaults are 10070-10079) and 50000-50199 (Horizon's DME UDP base 50000). */
void kz3_net_config_defaults(Kz3NetConfig* cfg);

/* Parses `len` bytes of online.ini text over `cfg`. Top-level keys: host, ports, region (the default server),
 * online_id, ticket_file, server (which section to use). A "[server NAME]" line starts a server entry (NAME: 1-32
 * letters, digits, spaces, '-', '_', '.'; at most 8 entries, names unique ignoring case) whose keys are host, ports
 * and region; any other [section] line goes back to the top level. '#'/';' start a comment outside double quotes;
 * a value in double quotes keeps its '#', ';' and edge spaces. Returns the number of rejected lines; each is reported
 * through `log` (may be NULL). The format is in the RE notes section 14.2. */
int kz3_net_parse_config(const char* text, size_t len, Kz3NetConfig* cfg, kz3_net_log_fn log);

/* Picks the server in use: the section named by `server = NAME` (case-insensitive), else the top-level host if one
 * is set, else the first section. Copies its host, ports and region to the top-level fields. Returns 0, or -1 when
 * `server` names no section (logged; the top-level fields stay as they are). */
int kz3_net_config_select(Kz3NetConfig* cfg, kz3_net_log_fn log);

/* 1 when s (NUL-terminated) is a valid region label: well-formed UTF-8 (no overlong forms, no surrogates), code
 * points U+0020..U+FFFD without the C1 controls U+007F..U+009F, 1..KZ3_NET_LABEL_MAX_CHARS characters, no '"'. */
int kz3_net_valid_region_label(const char* s);

/* Writes s with & < > " ' as XML entities (the GUM parser's amp/lt/gt/quot/apos) to out, NUL-terminated. Returns the
 * length written, or 0 when it does not fit in cap (out is then ""). */
size_t kz3_net_xml_escape(const char* s, char* out, size_t cap);

/* The region label for a localisation key, for the Localizer hook (kz3_net_hooks.h): the label of the server in use
 * when online, a label is configured and key[0..len) is "menu_region_" followed by 1-9 digits; NULL otherwise. */
const char* kz3_net_region_label_for_key(const char* key, size_t len);

/* Puts the config into the network policy: strict mode, MUIS host -> server_ip, telemetry denied, server ports
 * allowed. Returns 0, or -1 if the policy tables refused an entry. */
int kz3_net_apply(const Kz3NetConfig* cfg, const uint8_t server_ip[4]);

/* loads the player's ticket file (bounded read, np_ticket_check) into np_kz3 and settles the online id:
 * the ticket's id is used when online_id is empty; a different configured id refuses the file. Returns 0, or -1
 * (logged; the player is then not signed in, so nothing is sent with a wrong identity). */
int kz3_net_load_ticket(const char* path, Kz3NetConfig* cfg, kz3_net_log_fn log);

/* Test-only loopback capture (plan step 5, rt_capture.c): KZ3_NET_CAPTURE=1 with the server at 127.0.0.1 starts a
 * listener on 127.0.0.1:10071 (MUIS) and one on 10075 (MAS, pass 6) inside the game process that log the RT frames
 * the game sends and answer up to TicketLogin. One call per port (at most 4); online_id may be NULL. */
int kz3_net_capture_start(uint16_t port, const char* hello_file, const char* online_id);
/* The same with a kind: http = 1 starts the SVO HTTP server (request line and header names logged; region list and
 * nplogin.do answered, 404 otherwise). */
int kz3_net_capture_start_kind(uint16_t port, const char* hello_file, const char* online_id, int http);
/* The region label the capture's region list carries in <name> (escaped); NULL or "" = none. Call before start. */
void kz3_net_capture_set_region_label(const char* label);

/* Called once from kz3_hle_register (port/kz3/hle_kz3.cpp). Offline: does nothing. */
void kz3_net_register(void);

#ifdef __cplusplus
}
#endif

#endif /* KZ3_NET_H */
