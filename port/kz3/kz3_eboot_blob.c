/* The player's EBOOT.elf as bytes of kz3.exe (release step 9s; see kz3_embed.h).
 *
 * CMake (port/kz3/CMakeLists.txt, -DKZ3_EMBED_EBOOT=<path>) writes kz3_eboot_embed.h into the build folder with
 * KZ3_EMBED_EBOOT_FILE (the path, forward slashes) and KZ3_EMBED_EBOOT_SHA256 (its hash at configure time). The
 * hash being in this translation unit also keeps ccache honest: .incbin'd bytes are not part of the preprocessed
 * text ccache hashes, but a changed EBOOT changes the hash define and so the cache key. Without the header (a
 * developer build) this file defines an accessor that returns NULL. */
#include "kz3_embed.h"

#if defined(__has_include)
#if __has_include("kz3_eboot_embed.h")
#include "kz3_eboot_embed.h"
#endif
#endif

#if defined(KZ3_EMBED_EBOOT_FILE) && defined(KZ3_EMBED_EBOOT_SHA256)

#define KZ3_STR2(x) #x
#define KZ3_STR(x) KZ3_STR2(x)
#define KZ3_SYM(n) KZ3_STR(__USER_LABEL_PREFIX__) #n

#if defined(_WIN32)
#define KZ3_RODATA ".section .rdata,\"dr\"\n"
#elif defined(__APPLE__)
#define KZ3_RODATA ".const\n"
#else
#define KZ3_RODATA ".section .rodata\n"
#endif

/* clang-format off */
__asm__(KZ3_RODATA
        ".balign 64\n"
        ".globl " KZ3_SYM(kz3_eboot_blob) "\n"
        KZ3_SYM(kz3_eboot_blob) ":\n"
        ".incbin \"" KZ3_EMBED_EBOOT_FILE "\"\n"
        ".globl " KZ3_SYM(kz3_eboot_blob_end) "\n"
        KZ3_SYM(kz3_eboot_blob_end) ":\n"
        ".byte 0\n"
        ".text\n");
/* clang-format on */

extern const unsigned char kz3_eboot_blob[];
extern const unsigned char kz3_eboot_blob_end[];

const char* kz3_embedded_eboot(const unsigned char** data, size_t* size)
{
    *data = kz3_eboot_blob;
    *size = (size_t)(kz3_eboot_blob_end - kz3_eboot_blob);
    return KZ3_EMBED_EBOOT_SHA256;
}

#else

const char* kz3_embedded_eboot(const unsigned char** data, size_t* size)
{
    *data = NULL;
    *size = 0;
    return NULL;
}

#endif
