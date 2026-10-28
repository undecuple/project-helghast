/* host test, part 2: an ordinary runtime-style translation unit compiled with
 *   -DKZ3_LOG_REDIRECT_STDIO -include port/kz3/kz3_log.h
 * exactly as the build lane will compile the ps3recomp runtime. Its printf / fprintf(stderr) / puts / fputs /
 * fflush calls must reach the logger; fprintf to a real file must not. Checked by test_kz3_log.c (t_router). */
#include <stdio.h>
#include <string.h>

void redirect_tu_run(const char* root)
{
    printf("[redir] printf %d\n", 42);
    fprintf(stderr, "[hle] unresolved NID 0x%08X\n", 0xDEADBEEFu);
    puts("[redir] puts");
    fputs("[redir] fputs\n", stderr);
    fflush(stderr);
    printf("[mfc-list] REDIRHIDDEN %d\n", 1);
    char p[700];
    snprintf(p, sizeof p, "%s/redir.txt", root);
    FILE* f = fopen(p, "w");
    if (f) { fprintf(f, "to-a-real-file %d\n", 7); fflush(f); fclose(f); }
}
