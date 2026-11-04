/* Host test for patch 2151 (libs/video/rsx_menu_cols.h): the Display page's value column for long labels. No game data. */
#include "rsx_menu_cols.h"
#include <assert.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static const char page[] =
        "<list id=\"lst_options\" class=\"PEDS\" height=\"400\">\n"
        "<select id=\"a\" label=\"@key{x}\">\n<selectitem value=\"1\">a</selectitem></select>\n"
        "<select id=\"b\" label=\"y\" width=\"555\" optionwidth=\"250\">\n"
        "<select\n id=\"c\">\n</list>\n";
    int w = 0, o = 0;
    size_t n = 0;
    char* r;
    CHECK(rsx_mc_lookup(1, &w, &o) == 0);          /* English keeps the game's layout */
    CHECK(rsx_mc_lookup(12, &w, &o) == 1 && w - o >= 265 && o >= 150);   /* Russian: wider label column (pass 142: one box, may be a little narrower) */
    CHECK(rsx_mc_lookup(99, &w, &o) == 0);
    r = rsx_mc_apply(page, sizeof page - 1, &n, 500, 225);
    CHECK(r && n == strlen(r));
    if (r) {
        const char* p = r;
        int count = 0;
        while ((p = strstr(p, "optionwidth=\"225\"")) != NULL) { count++; p++; }
        CHECK(count == 2);                                         /* selects a and c; b keeps its own 555/250 */
        CHECK(strstr(r, "width=\"555\" optionwidth=\"250\"") != NULL);
        CHECK(strstr(r, "<selectitem value=\"1\">") != NULL);     /* selectitem is not a select */
        CHECK(strstr(r, "class=\"PEDS\" height=\"400\" width=\"532\">") == NULL);   /* the width goes right after the list id tag start */
        CHECK(strstr(r, "<list id=\"lst_options\" width=\"532\" class=\"PEDS\"") != NULL);
        free(r);
    }
    CHECK(rsx_mc_apply("<panel/>", 8, &n, 500, 225) == NULL);       /* nothing to add: NULL */
    /* RSX pass 142: one value box per language; ids make no difference (the game's pages have one box per page) */
    {
        static const char pg2[] =
            "<list id=\"lst_options\" class=\"PEDS\">\n<select id=\"slc_kz3_display\" label=\"a\">\n"
            "<select id=\"slc_kz3_unfocused\">\n<select id=\"x\">\n</list>\n";
        size_t i;
        CHECK(rsx_mc_lookup(12, &w, &o) == 1 && w == 500 && o == 190);
        r = rsx_mc_apply(pg2, sizeof pg2 - 1, &n, w, o);
        CHECK(r != NULL);
        if (r) {
            const char* p = r;
            int count = 0;
            while ((p = strstr(p, "optionwidth=\"190\"")) != NULL) { count++; p++; }
            CHECK(count == 3);                                                        /* all three selects share the box */
            CHECK(strstr(r, "<select width=\"500\" optionwidth=\"190\" id=\"slc_kz3_display\"") != NULL);
            free(r);
        }
        CHECK(rsx_mc_lookup(1, &w, &o) == 0);
        for (i = 0; i < sizeof rsx_mc_display / sizeof rsx_mc_display[0]; i++)
            CHECK(rsx_mc_display[i].width == 500 && rsx_mc_display[i].option >= 90 && 478 - rsx_mc_display[i].option >= 250);
    }
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
