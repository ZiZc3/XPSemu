/*
 * Host test of native/kernel/rtl_case.h, the uppercase the runtime
 * library's upcase exports give (RtlUpcaseUnicodeChar and friends).
 *
 *   gcc -O1 -I<repo> tests/native/rtl/case_test.c -o case_test && ./case_test
 */
#include "native/kernel/rtl_case.h"

#include <stdio.h>

static int g_fail;

static void up(uint16_t c, uint16_t want, const char *what)
{
    const uint16_t got = nc_rtl_upcase(c);
    const int ok = got == want;
    g_fail += !ok;
    printf("%s %s U+%04X -> U+%04X%s", ok ? "PASS" : "FAIL", what, c, want, ok ? "\n" : "");
    if (!ok) {
        printf(" (got U+%04X)\n", got);
    }
}

int main(void)
{
    up('S', 'S', "ASCII upper");             /* Midtown Madness 3's call */
    up('s', 'S', "ASCII lower");
    up('a', 'A', "first");
    up('z', 'Z', "last");
    up('0', '0', "digit");
    up('{', '{', "after z");
    up('`', '`', "before a");
    up(0xE9, 0xC9, "e acute");
    up(0xF7, 0xF7, "division sign");
    up(0xFF, 0x178, "y diaeresis");
    up(0xB5, 0x39C, "micro sign");
    up(0xDF, 0xDF, "sharp s (no single capital)");
    up(0x101, 0x100, "a macron");
    up(0x100, 0x100, "A macron");
    up(0x13A, 0x139, "l acute");
    up(0x142, 0x141, "l stroke");
    up(0x153, 0x152, "oe");
    up(0x17E, 0x17D, "z caron");
    up(0x138, 0x138, "kra");
    up(0x3B1, 0x391, "alpha");
    up(0x3C2, 0x3A3, "final sigma");
    up(0x3C9, 0x3A9, "omega");
    up(0x430, 0x410, "cyrillic a");
    up(0x44F, 0x42F, "cyrillic ya");
    up(0x451, 0x401, "cyrillic yo");
    up(0xFF41, 0xFF21, "fullwidth a");
    up(0x3042, 0x3042, "hiragana");
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
