/* kz3_ini.h: kz3.ini next to kz3.exe, so the game starts with no launch options (release pass 26).
 *
 * Written by the first-run build (tools/release/firstrun.py -> launch.write_game_ini); hand-editable. Format:
 *
 *   [game]
 *   eboot = D:\Games\BCUS98234\USRDIR\EBOOT.elf     ; argv[1] when kz3.exe gets no argument
 *   vfs_root = ...      ; PS3_VFS_ROOT: the folder that holds PS3_GAME (the dump, read in place)
 *   cache_root = ...    ; KZ3_CACHE_ROOT: every write (inflated archive entries psarc-cache, /dev_hdd1 cache); empty = vfs_root
 *   font_dir = ...    ; KZ3_FONT_DIR (the firmware's dev_flash/data/font)
 *   lang = ...          ; KZ3_LANG            update_dir, hdd0_root, edat_cache, shader_cache (the launcher's paths)
 *   data_dir = ...      ; working folder (where the saves' ./gamedata lands)
 *   [display]
 *   window_size = 1080p ; KZ3_WINDOW_SIZE (WxH or 720p/900p/1080p/1440p/2160p/4k)
 *
 * Rules: "key = value" lines; section headers, blank lines and lines starting with ';' or '#' are skipped; keys are
 * case-insensitive and unique across sections (the first non-empty one counts); a value may be in double quotes; an
 * empty value counts as unset. An environment variable that is already set wins over its key, and an explicit argv[1] wins over
 * `eboot` (tests and the developer scripts keep working). A relative path is relative to the exe's folder.
 *
 * Header-only and free of ps3recomp/Windows types so a host test can include it (tools/release/test_kz3_ini.py). */
#ifndef KZ3_INI_H
#define KZ3_INI_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* key;
    const char* env; /* the environment variable the key stands for; NULL = handled by main.cpp itself */
    int path;        /* 1 = a path (relative = relative to the exe's folder) */
} kz3_ini_key_t;

static const kz3_ini_key_t kz3_ini_keys[] = {
    {"eboot", NULL, 1},
    {"data_dir", NULL, 1},
    {"vfs_root", "PS3_VFS_ROOT", 1},
    {"cache_root", "KZ3_CACHE_ROOT", 1}, /* round KZ3-03: the writable root; unset = vfs_root */
    {"font_dir", "KZ3_FONT_DIR", 1},
    {"lang", "KZ3_LANG", 0},
    {"window_size", "KZ3_WINDOW_SIZE", 0},
    {"update_dir", "KZ3_UPDATE_DIR", 1},
    {"hdd0_root", "PS3_HDD0_ROOT", 1},
    {"edat_cache", "PS3_EDAT_CACHE", 1},
    {"shader_cache", "RSX_SHADER_CACHE_DIR", 1},
    {"mouse_look", "KZ3_MOUSE_LOOK", 0}, /* patch 2097: default on, 0 = off */
    {"discord_presence", "KZ3_DISCORD_PRESENCE", 0},   /* net rounds 33/42: Discord Rich Presence, 1 = on (default off); the Options > Display row writes it */
    {"discord_client_id", "KZ3_DISCORD_CLIENT_ID", 0}, /* the Discord application id; empty = the project's own */
};
#define KZ3_INI_NKEYS (sizeof kz3_ini_keys / sizeof kz3_ini_keys[0])

/* The whole file into buf (NUL-terminated); -> bytes read, or -1 when it cannot be opened. */
static int kz3_ini_read_file(const char* path, char* buf, size_t cap)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* The value of `key` in the ini text -> out; 1 = found and not empty, 0 = absent or empty. */
static int kz3_ini_get(const char* text, const char* key, char* out, size_t cap)
{
    const char* p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3; /* BOM */
    const size_t klen = strlen(key);
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        const char* s = p;
        while (s < eol && (*s == ' ' || *s == '\t')) s++;
        const char* eq = s;
        while (eq < eol && *eq != '=') eq++;
        if (s < eol && *s != ';' && *s != '#' && *s != '[' && eq < eol) {
            const char* ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            int same = (size_t)(ke - s) == klen;
            for (size_t i = 0; same && i < klen; i++)
                same = tolower((unsigned char)s[i]) == tolower((unsigned char)key[i]);
            if (same) {
                const char* v = eq + 1;
                const char* ve = eol;
                while (v < ve && (*v == ' ' || *v == '\t')) v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                if (ve - v >= 2 && *v == '"' && ve[-1] == '"') {
                    v++;
                    ve--;
                }
                size_t n = (size_t)(ve - v);
                if (n >= cap) return 0; /* too long: refused, not cut */
                if (n > 0) {            /* empty = unset: a later line with the same key may give it */
                    memcpy(out, v, n);
                    out[n] = 0;
                    return 1;
                }
            }
        }
        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
    return 0;
}

/* A path value as an absolute path: kept when absolute (C:\, C:/, \\server, /x), else base_dir/value. 0 = too long. */
static int kz3_ini_resolve(const char* base_dir, const char* value, char* out, size_t cap)
{
    int abs = value[0] == '/' || value[0] == '\\' || (isalpha((unsigned char)value[0]) && value[1] == ':');
    int n = abs ? snprintf(out, cap, "%s", value) : snprintf(out, cap, "%s/%s", base_dir, value);
    return n > 0 && (size_t)n < cap;
}

#endif /* KZ3_INI_H */
