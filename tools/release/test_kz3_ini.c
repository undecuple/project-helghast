/* Host driver for port/kz3/kz3_ini.h (release pass 26), run by test_kz3_ini.py:
 *   test_kz3_ini dump FILE          every key of kz3_ini_keys found in FILE as "key=value" lines (the runtime's view)
 *   test_kz3_ini resolve BASE VALUE  kz3_ini_resolve's result
 *   test_kz3_ini long               a value longer than the buffer is refused (prints "refused") */
#include <stdio.h>
#include <string.h>

#include "kz3_ini.h"

int main(int argc, char** argv)
{
    static char text[16384];
    char v[1024], out[1100];
    if (argc == 3 && !strcmp(argv[1], "dump")) {
        if (kz3_ini_read_file(argv[2], text, sizeof text) < 0) {
            puts("nofile");
            return 0;
        }
        for (size_t i = 0; i < KZ3_INI_NKEYS; i++)
            if (kz3_ini_get(text, kz3_ini_keys[i].key, v, sizeof v)) printf("%s=%s\n", kz3_ini_keys[i].key, v);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "resolve")) {
        if (!kz3_ini_resolve(argv[2], argv[3], out, sizeof out)) {
            puts("toolong");
            return 0;
        }
        puts(out);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "long")) {
        char small[8];
        puts(kz3_ini_get("eboot = 0123456789\n", "eboot", small, sizeof small) ? "taken" : "refused");
        return 0;
    }
    return 2;
}
