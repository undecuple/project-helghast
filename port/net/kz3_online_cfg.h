/*
 * KZ3 port: the online method files (network pass 17, rules of 2026-11-09/10).
 *
 * Exactly one online method at a time: re-psn, a community PSN-replacement server, or off. Two separate files, never mixed:
 *   repsn.ini      [repsn]   enabled, server (host[:port], default port 36100), name (online ID, 3-16 of A-Za-z0-9_-),
 *                            comm_id (default NPWR01569_00, the game's own), p2p_port, report_rtt
 *   community.ini  [community] enabled, active (the picked preset's name); [preset NAME] address (host[:port]), name
 * Each kind is read from the shared (player-wide) folder first, then from the game's own folder; a later key overrides
 * an earlier one, presets merge by name. Player-wide values (enabled, server, name, presets) belong in the shared
 * file, per-game values (comm_id, p2p_port, report_rtt) in the game's.
 * No preset and no host ships; this code and its tests name no community server.
 *
 * Pure C with no ps3recomp dependency (host tests build it alone). Every byte of every file is untrusted.
 */
#ifndef KZ3_ONLINE_CFG_H
#define KZ3_ONLINE_CFG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KZ3_OC_REPSN_DEFAULT_PORT 36100
#define KZ3_OC_COMMUNITY_DEFAULT_PORT 10071   /* MUIS port the game's client uses; ASSUMPTION for an address without a port */
#define KZ3_OC_COMM_ID "NPWR01569_00"         /* Killzone 3's own NP communication ID */
#define KZ3_OC_MAX_PRESETS 16
#define KZ3_OC_PRESET_NAME_MAX 32
#define KZ3_OC_FILE_MAX (64 * 1024)

typedef struct {
    char     name[KZ3_OC_PRESET_NAME_MAX + 1];   /* the preset's name (1-32 of letters, digits, space, - _ .) */
    char     host[254];
    uint16_t port;
    char     online_name[17];                    /* the online ID used on this server; "" = none set */
} Kz3OcPreset;

typedef struct {
    int      repsn_enabled;
    char     repsn_host[254];
    uint16_t repsn_port;                 /* 0 = no server set */
    char     repsn_name[17];
    char     comm_id[13];
    uint16_t p2p_port;                   /* 0 = none */
    int      report_rtt;
    int      community_enabled;
    char     active[KZ3_OC_PRESET_NAME_MAX + 1];   /* "" = none picked */
    Kz3OcPreset presets[KZ3_OC_MAX_PRESETS];
    int      npresets;
    int      rejected;                   /* lines rejected over all files read */
} Kz3OcConfig;

typedef void (*kz3_oc_log_fn)(const char* msg);

typedef enum { KZ3_OC_OFF = 0, KZ3_OC_REPSN = 1, KZ3_OC_COMMUNITY = 2, KZ3_OC_REFUSED = 3 } Kz3OcMethod;
typedef enum { KZ3_ROUTE_GAME = 0,        /* the game's own Medius flow (guest sockets) */
               KZ3_ROUTE_PORT = 1 } Kz3OcRoute;   /* any feature the port adds (host sockets, psnr) */

typedef struct {
    Kz3OcMethod method;
    char     host[254];          /* the one server of the session ("" when off or refused) */
    uint16_t port;
    char     online_name[17];
    char     comm_id[13];
    uint16_t p2p_port;
    int      report_rtt;
    int      port_features;      /* 1 only for re-psn */
    char     other_hosts[KZ3_OC_MAX_PRESETS][254];   /* community hosts known to the files; unreachable in a re-psn session */
    int      nother;
    char     log_line[200];      /* the clear log line */
    char     player_message[200];/* the player-facing message when refused or when port features are off */
} Kz3OcSession;

void kz3_oc_defaults(Kz3OcConfig* c);
int  kz3_oc_valid_online_name(const char* s);                       /* 3-16 of A-Za-z0-9_- */
int  kz3_oc_valid_host(const char* s);                              /* DNS name or dotted IPv4, no port */
int  kz3_oc_parse_hostport(const char* s, unsigned default_port, char host[254], uint16_t* port);  /* 1 ok */
int  kz3_oc_valid_comm_id(const char* s);                           /* NPWR + 5 digits + _ + 2 digits */
int  kz3_oc_valid_preset_name(const char* s);

/* Parse one file's text over c (layering: call for the shared file, then the game's). Returns the lines rejected
 * (also added to c->rejected). The repsn parser rejects [community]/[preset] lines and the reverse (never mixed). */
int  kz3_oc_parse_repsn(const char* text, size_t len, Kz3OcConfig* c, kz3_oc_log_fn log);
int  kz3_oc_parse_community(const char* text, size_t len, Kz3OcConfig* c, kz3_oc_log_fn log);

/* Preset editing (what the menu and the launcher call). 0 ok, -1 refused (bad value, duplicate, full, unknown). */
int  kz3_oc_preset_add(Kz3OcConfig* c, const char* name, const char* address, const char* online_name);
int  kz3_oc_preset_edit(Kz3OcConfig* c, const char* name, const char* address, const char* online_name);
int  kz3_oc_preset_delete(Kz3OcConfig* c, const char* name);        /* deleting the active one clears it */
int  kz3_oc_preset_pick(Kz3OcConfig* c, const char* name);          /* one active at a time */

/* Method choice (the toggle): choosing one switches the other off. KZ3_OC_OFF switches both off. */
void kz3_oc_set_method(Kz3OcConfig* c, Kz3OcMethod m);

/* The session: off / re-psn / community / refused (both enabled, or the chosen method incomplete). Never silent. */
void kz3_oc_select(const Kz3OcConfig* c, Kz3OcSession* s);

/* Egress: 0 = allowed. Community session: the game route reaches the preset host only, the port route is refused.
 * re-psn session: both routes reach the re-psn host only; a community host is refused. Off/refused: everything. */
int  kz3_oc_egress_check(const Kz3OcSession* s, Kz3OcRoute route, const char* host, unsigned port);

/* Text for the files (the single serialisers; the community file is all player-wide). Return length, or 0 when it does not fit. */
#define KZ3_OC_SCOPE_SHARED 0   /* player-wide keys: enabled, server, name */
#define KZ3_OC_SCOPE_GAME   1   /* per-game keys: comm_id, p2p_port, report_rtt */
size_t kz3_oc_format_repsn(const Kz3OcConfig* c, int scope, char* out, size_t cap);
size_t kz3_oc_format_community(const Kz3OcConfig* c, char* out, size_t cap);

/* The single writer: temp file in the same folder, flush, atomic replace. 0 ok, -1 failed (the old file is kept). */
int  kz3_oc_write_file(const char* path, const char* data, size_t len);
/* Bounded read of a whole file (NULL when absent/too big/short); caller frees. */
char* kz3_oc_read_file(const char* path, size_t* len);

/* The shared folder (player-wide files for every port): KZ3_SHARED_DIR if set, else PS3RECOMP_SHARED_DIR (J-Stars' name), else Windows %APPDATA%\PS3Recomp,
 * else $XDG_CONFIG_HOME/ps3recomp or ~/.config/ps3recomp. Portable: <portable_dir>/shared when portable_dir != NULL.
 * Returns the length, 0 if no folder can be named. */
size_t kz3_oc_shared_dir(const char* portable_dir, char* out, size_t cap);

/* Reads both kinds from shared_dir then game_dir (either may be NULL). Returns the number of files found. */
int  kz3_oc_load(const char* shared_dir, const char* game_dir, Kz3OcConfig* c, kz3_oc_log_fn log);

#ifdef __cplusplus
}
#endif
#endif
