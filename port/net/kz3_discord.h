/* kz3_discord: Discord Rich Presence over Discord's local IPC (ENHANCEMENT, opt-in, network pass 33).
 * Our own small client of the documented protocol (no SDK, no third-party code). Everything here except
 * kz3_discord_start/stop/note_open's Windows side is pure and host-tested (tools/net/test_discord.c). */
#ifndef KZ3_DISCORD_H
#define KZ3_DISCORD_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { KD_OP_HANDSHAKE = 0, KD_OP_FRAME = 1, KD_OP_CLOSE = 2, KD_OP_PING = 3, KD_OP_PONG = 4 };
#define KD_MAX_PAYLOAD 65536u

/* Frame = u32 LE opcode + u32 LE length + JSON. Returns bytes written, 0 when it does not fit. */
size_t kd_frame_encode(uint32_t op, const char* json, size_t json_len, uint8_t* out, size_t cap);
/* Returns bytes consumed (header + payload) and sets *op / *payload / *plen; 0 = incomplete, -1 = hostile length. */
int kd_frame_decode(const uint8_t* buf, size_t len, uint32_t* op, const uint8_t** payload, uint32_t* plen);

/* JSON text; each returns the length or 0 when it does not fit (or the client id is not 1..32 digits). */
size_t kd_build_handshake(char* out, size_t cap, const char* client_id);
typedef struct {
    const char* details; /* NULL = omitted */
    const char* state;   /* NULL = omitted */
    int64_t start;       /* unix seconds; 0 = omitted */
} kd_activity_t;
size_t kd_build_activity(char* out, size_t cap, uint32_t pid, unsigned nonce, const kd_activity_t* a);
size_t kd_build_clear(char* out, size_t cap, uint32_t pid, unsigned nonce);

/* A guest path -> what the game is doing. Returns 1 when the path is a level-description lump (sets *kind and *name),
 * else 0. kind 1 = campaign chapter (name = display name, or "Campaign" for an unknown directory), 2 = multiplayer. */
int kd_classify_path(const char* guest_path, int* kind, char* name, size_t cap);
const char* kd_chapter_name(const char* dir); /* NULL when unknown */

/* Net pass 42: the Options > Display row "Discord status" (On/Off, saved as kz3.ini discord_presence).
 * kd_parse_onoff: "1"/"on"/"true"/"yes" = 1, "0"/"off"/"false"/"no" = 0 (any case), else -1.
 * kd_initial: the state at boot from the environment value (NULL/"" = unset) and the kz3.ini text (NULL = no file).
 * main.cpp copies discord_presence into KZ3_DISCORD_PRESENCE only when the variable is not set, so an environment value
 * that differs from the file's (absent key = 0) was set by the player for this run: *env_forced = 1. -> 0 or 1. */
int kd_parse_onoff(const char* s);
int kd_initial(const char* env, const char* ini_text, int* env_forced);
/* kz3.ini text with `discord_presence = val`: the key's first line replaced (any section, as kz3_ini_get reads it),
 * else a new line under [game], else a new [game] section at the end; every other byte kept.
 * -> bytes written (no NUL counted), -1 when cap is too small. */
int kd_ini_upsert(const char* in, char* out, size_t cap, const char* key, const char* val);
/* The row's bridge command (Lua "kb:discord [on|off]"). cmd is the text after "kb:". -> 0 when cmd is not ours;
 * 1 = query (reply "on"/"off" = *running); 2 = set: *want = 0/1, reply = the new state. A bad value is a query. */
int kd_cmd_parse(const char* cmd, int running, int* want, char* reply, size_t cap);

/* Options > Display gets the row: the page as main.cpp serves it (patch 2095/2110's rows, tooltip panel and Lua) in,
 * the page with the row before the Gamma link, its help text in gKz3Tip, its value on OnPageOn and Default = Off.
 * NULL when an anchor is missing (the page is then served without the row). Caller frees. */
char* kd_patch_display_gum(const char* in, size_t n, size_t* out_n);

/* Runtime (Windows named pipes; elsewhere these only log "off"). Reads KZ3_DISCORD_PRESENCE / KZ3_DISCORD_CLIENT_ID
 * (filled from kz3.ini keys discord_presence / discord_client_id) and the kz3.ini beside the exe.
 * kz3_discord_start: once at boot; installs the chapter tracker and the row's bridge command (patch 4025's
 * g_pad_kb_ext_cmd), and starts the presence when the boot state is on. kz3_discord_set(on): the row (thread-safe;
 * on = connect and send the activity, off = SET_ACTIVITY without activity, then disconnect; save = write kz3.ini).
 * kz3_discord_stop: at exit (clears the activity like off, no save). */
void kz3_discord_start(void);
void kz3_discord_set(int on, int save);
int  kz3_discord_running(void);
void kz3_discord_stop(void);

#ifdef __cplusplus
}
#endif
#endif
