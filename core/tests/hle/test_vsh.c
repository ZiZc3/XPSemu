#include "hw/xbox/hle/shader/vsh.h"
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

/* Instructions from the nv2a_vsh_cpu disassembler tests (comment = meaning). */
static const uint32_t k_prog[][4] = {
    { 0x00000000, 0x0420061B, 0x083613FC, 0x5011F818 }, /* MOV oD0, v3 + RCP R1.w, R1.w */
    { 0x00000000, 0x00EC801B, 0x0836186C, 0x20702800 }, /* DP4 oPos.z, v0, c[100] */
    { 0x00000000, 0x0047401A, 0xC434186C, 0x2070E800 }, /* MUL oPos.xyz, R12.xyz, c[58].xyz */
    { 0x00000000, 0x0400001B, 0x083613FC, 0x2070F82C }, /* RCP oFog, v0.w */
    { 0x00000000, 0x01A00000, 0x0436106C, 0x20700FF8 }, /* ARL A0, R0.x */
    { 0x00000000, 0x006F20BF, 0x9C001456, 0x7C000002 }, /* ADD R0.xy, c[A0+121].zw, -c[A0+121].xy */
    { 0x00000000, 0x0240081B, 0x1436186C, 0x2F20F824 }, /* MUL R2, R1, c[0] + MOV oD1, v4 */
    { 0x00000000, 0x0087601B, 0xC400286C, 0x3070E801 }, /* MAD oPos.xyz, R12, R1.x, c[59] (final) */
};
#define NSLOTS (sizeof(k_prog) / sizeof(k_prog[0]))

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.vert", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

static void test_program(void)
{
    HleVshKey k = { .in_mask = (1u << 0) | (1u << 3) | (1u << 4) };
    size_t n = hle_vsh_glsl(&k_prog[0][0], NSLOTS, &k, buf, sizeof(buf));
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("vsh_program");
    HT_CHECK(strstr(buf, "layout(location = 0) in vec4 v0_in;") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 3) in vec4 v3_in;") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 1) in vec4") == NULL);
    /* registers the arrays do not feed read their current values */
    HT_CHECK(strstr(buf, "vec4 v1 = vcur[1];") != NULL);
    HT_CHECK(strstr(buf, "{ vec4 c[192]; vec4 vcur[16]; }") != NULL);
    /* paired MAC + ILU: both computed before either writes */
    HT_CHECK(strstr(buf, "vec4 m = v3.xyzw;\n    vec4 i = rcp4((R1.wwww).x);\n"
                         "    oD0.xyzw = m.xyzw;\n    R1.w = i.w;") != NULL);
    HT_CHECK(strstr(buf, "oPos.z = m.z;") != NULL);
    HT_CHECK(strstr(buf, "mulz(oPos.xyzz, c[58].xyzz)") != NULL);   /* R12 reads oPos */
    HT_CHECK(strstr(buf, "oFog.x = i.w;") != NULL);     /* the top masked component */
    HT_CHECK(strstr(buf, "vFog = oFog.x;") != NULL);
    HT_CHECK(strstr(buf, "A0 = int(floor(m.x + 1.0 / 1024.0));") != NULL);
    HT_CHECK(strstr(buf, "c[clamp(A0 + 121, 0, 191)].zwww") != NULL);
    HT_CHECK(strstr(buf, "-c[clamp(A0 + 121, 0, 191)].xyyy") != NULL);
    HT_CHECK(strstr(buf, "oD1.xyzw = ") != NULL);
    HT_CHECK(strstr(buf, "(mulz(oPos.xyzw, R1.xxxx) + c[59].xyzw)") != NULL);
    HT_CHECK(strstr(buf, "gl_Position") != NULL);
    HT_CHECK(strstr(buf, "flat out") == NULL);
    k.flat = 1;
    HT_CHECK(hle_vsh_glsl(&k_prog[0][0], NSLOTS, &k, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "layout(location = 0) flat out vec4 vDiffuse;") != NULL);
}

/* S32K attributes arrive as ivec4, CMP as one packed int */
static void test_attr_kinds(void)
{
    HleVshKey k = { .in_mask = 0x19, .int_mask = 0x08, .cmp_mask = 0x10 };
    HT_CHECK(hle_vsh_glsl(&k_prog[0][0], NSLOTS, &k, buf, sizeof(buf)) > 0);
    dump("vsh_kinds");
    HT_CHECK(strstr(buf, "layout(location = 0) in vec4 v0_in;") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 3) in ivec4 v3_in;") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 4) in uvec2 v4_in;") != NULL);
    HT_CHECK(strstr(buf, "int v4p = int(v4_in.x | (v4_in.y << 16u));") != NULL);
    HT_CHECK(strstr(buf, "vec4 v3 = vec4(v3_in);") != NULL);
    HT_CHECK(strstr(buf, "bitfieldExtract(v4p, 22, 10)") != NULL);
}

/* a program without a FINAL instruction is rejected */
static void test_no_final(void)
{
    HleVshKey k = { .in_mask = 1 };
    HT_CHECK(hle_vsh_glsl(&k_prog[0][0], NSLOTS - 1, &k, buf, sizeof(buf)) == 0);
}

/* stops at the FINAL instruction (trailing garbage ignored) */
static void test_stops_at_final(void)
{
    uint32_t t[NSLOTS + 1][4];
    memcpy(t, k_prog, sizeof(k_prog));
    memset(t[NSLOTS], 0xFF, sizeof(t[NSLOTS]));
    HleVshKey k = { .in_mask = 1 };
    size_t a = hle_vsh_glsl(&t[0][0], NSLOTS + 1, &k, buf, sizeof(buf));
    HT_CHECK(a > 0);
    HT_CHECK(strstr(buf, "slot 8") == NULL);
}

/* a program writing no oFog is not fogged: factor 1 under FOGTABLEMODE
 * NONE, distance 0 under the table modes (Halo's Bink movie quad) */
static void test_unwritten_fog(void)
{
    uint32_t t[NSLOTS][4];
    memcpy(t, k_prog, sizeof(k_prog));
    memset(t[3], 0, sizeof(t[3]));              /* RCP oFog -> NOP */
    HleVshKey k = { .in_mask = 1 };
    HT_CHECK(hle_vsh_glsl(&t[0][0], NSLOTS, &k, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog = 1.0;") != NULL);
    HT_CHECK(strstr(buf, "oFog.x =") == NULL);
    k.fog_table = 1;
    HT_CHECK(hle_vsh_glsl(&t[0][0], NSLOTS, &k, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog = 0.0;") != NULL);
}

/* a buffer too small fails instead of truncating */
static void test_overflow(void)
{
    HleVshKey k = { .in_mask = 1 };
    HT_CHECK(hle_vsh_glsl(&k_prog[0][0], NSLOTS, &k, buf, 512) == 0);
}

int main(void)
{
    HT_RUN(test_program);
    HT_RUN(test_attr_kinds);
    HT_RUN(test_no_final);
    HT_RUN(test_stops_at_final);
    HT_RUN(test_unwritten_fog);
    HT_RUN(test_overflow);
    HT_DONE();
}
