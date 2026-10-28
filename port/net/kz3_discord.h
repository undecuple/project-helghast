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

/* Runtime (Windows named pipes; elsewhere these only log "off"). Reads KZ3_DISCORD_PRESENCE / KZ3_DISCORD_CLIENT_ID
 * (filled from kz3.ini keys discord_presence / discord_client_id). */
void kz3_discord_start(void);
void kz3_discord_stop(void);

#ifdef __cplusplus
}
#endif
#endif
