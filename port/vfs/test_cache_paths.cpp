/* Synthetic archive -> actual VFS hook -> cache -> readback. No game data.
 * The linker wrappers model legacy Win32's absolute MAX_PATH boundary on Linux. */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PSARC_SOURCE
#define PSARC_SOURCE "../kz3/psarc_vfs.cpp"
#endif
#include PSARC_SOURCE

extern "C" {
void (*g_ps3_vfs_miss_hook)(char*, size_t) = nullptr;
int (*g_ps3_fs_stat_hook)(const char*) = nullptr;
uint8_t* vm_base = nullptr;
int stbi_zlib_decode_buffer(char*, int, const char*, int) { return -1; }
const char* edat_resolve(const char* path, char*, size_t) { return path; }
void edat_set_cache_dir(const char*) {}
void edat_add_klicensee(const uint8_t*) {}

FILE* __real_fopen(const char*, const char*);
int __real_stat(const char*, struct stat*);
int __real_rename(const char*, const char*);
}

static unsigned blocked;
static bool too_long(const char* path)
{
    char cwd[2048];
    std::string absolute = path;
    if (absolute.empty() || absolute[0] != '/') {
        if (!getcwd(cwd, sizeof cwd)) std::abort();
        absolute = std::string(cwd) + '/' + absolute;
    }
    if (absolute.size() < 260) return false;
    blocked++;
    errno = ENAMETOOLONG;
    return true;
}
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
    return too_long(path) ? nullptr : __real_fopen(path, mode);
}
extern "C" int __wrap_stat(const char* path, struct stat* st)
{
    return too_long(path) ? -1 : __real_stat(path, st);
}
extern "C" int __wrap_rename(const char* from, const char* to)
{
    return too_long(from) || too_long(to) ? -1 : __real_rename(from, to);
}

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    setenv("KZ3_PATCH_SET", "none", 1);
    if (kz3_psarc_vfs_init(argv[1], argv[2]) < 0) return 3;
    if (!std::strcmp(argv[4], "added")) {
        kz3_overlay_config cfg = {};
        cfg.added_psarc = argv[1]; cfg.cache_dir = argv[2]; cfg.inflate = stbi_zlib_decode_buffer;
        if (kz3_overlay_init(&cfg)) return 9;
        g_overlay_on = true;
    }
#ifdef CHECK_CACHE_HELPER
    char small[2] = {'x', 'x'}, a[128], b[128];
    if (kz3_cache_file_path("root", "one/file.core", small, sizeof small) != -1 || small[0]) return 10;
    if (kz3_cache_file_path("root", "one/file.core", a, sizeof a) ||
        kz3_cache_file_path("root", "two/file.core", b, sizeof b) || !std::strcmp(a, b)) return 11;
#endif
    const std::string key = argv[3];
    std::string first;
    for (int n = 0; n < 2; n++) {
        char path[1200];
        std::string request = key;
        if (n) for (char& c : request) { if (c == '/') c = '\\'; else if (c >= 'a' && c <= 'z') c -= 32; }
        std::snprintf(path, sizeof path, "./PS3_GAME/USRDIR/%s", request.c_str());
        g_ps3_vfs_miss_hook(path, sizeof path);
        FILE* f = fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "FAIL: asset not readable; blocked=%u\n", blocked); return 4; }
        char body[64] = {0}; size_t size = fread(body, 1, sizeof body - 1, f); fclose(f);
        if (std::string(body, size) != "synthetic ISA_M4_Revolver") return 5;
        if (n && first != path) return 6;
        const size_t dot = key.rfind('.');
        if (dot != std::string::npos && key.substr(dot) == ".edat" &&
            std::string(path).substr(std::strlen(path) - 5) != ".edat") return 8;
        first = path;
        if (!n) { kz3_sf_close(g_arc.f); g_arc.f = nullptr; } /* second read must use the cache */
    }
    struct stat st;
    if (stat(first.c_str(), &st) || st.st_size != 25) return 7;
    std::printf("PASS: cold/warm case aliases read correct bytes; resolved=%s; blocked=%u\n", first.c_str(), blocked);
    return 0;
}
