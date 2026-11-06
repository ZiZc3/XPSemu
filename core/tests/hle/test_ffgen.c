#include "hw/xbox/hle/shader/ff.h"
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[32768];

static void dump(const char *name, const char *ext)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/ff_%s.%s", dir, name, ext);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* PGR2's 2D UI format: XYZRHW | DIFFUSE | TEX1, texture modulated by diffuse */
static void test_ui_quad(void)
{
    HleFfVsKey vk = { .fvf = 0x144, .ntex = 1 };
    size_t n = hle_ff_vs_glsl(&vk, buf, sizeof(buf));
    HT_CHECK(n > 0);
    HT_CHECK(strstr(buf, "1.0 / iPos.w") != NULL);           /* screen space */
    HT_CHECK(strstr(buf, "t = vec4(iTex0.xy, 0.0, 1.0)") != NULL);
    HT_CHECK(strstr(buf, "vTex0 = t") != NULL);
    HT_CHECK(strstr(buf, "iDiffuse") != NULL);
    HT_CHECK(strstr(buf, "FfConst") == NULL);                 /* no uniform block */
    dump("ui", "vert");
    HleFfPsKey pk = { .nstages = 1, .alpha_test = 1, .alpha_func = 6 };
    pk.st[0] = (HleFfStage){ .colorop = 4, .colorarg1 = 2, .colorarg2 = 0,
                             .alphaop = 4, .alphaarg1 = 2, .alphaarg2 = 0, .resultarg = 1,
                             .texture = 1 };
    n = hle_ff_ps_glsl(&pk, buf, sizeof(buf));
    HT_CHECK(n > 0);
    HT_CHECK(strstr(buf, "(t0 * vDiffuse)") != NULL);        /* MODULATE TEXTURE, DIFFUSE */
    HT_CHECK(strstr(buf, "a >= r") != NULL);                  /* GREATEREQUAL */
    HT_CHECK(strstr(buf, "discard") != NULL);
    dump("ui", "frag");
}

/* no descriptor indexing: the bindless arrays as the draw set's places */
static void test_fixed_slots(void)
{
    HleFfPsKey pk = { .nstages = 1 };
    pk.st[0] = (HleFfStage){ .colorop = 4, .colorarg1 = 2, .colorarg2 = 0,
                             .alphaop = 4, .alphaarg1 = 2, .alphaarg2 = 0, .resultarg = 1,
                             .texture = 1 };
    size_t n = hle_ff_ps_glsl(&pk, buf, sizeof(buf));
    HT_CHECK(n > 0 && strstr(buf, "nonuniformEXT(") != NULL);
    size_t m = hle_glsl_fixed_slots(buf, sizeof(buf));
    HT_CHECK(m > 0 && m == strlen(buf));
    HT_CHECK(strstr(buf, "nonuniform") == NULL);
    HT_CHECK(strstr(buf, "uniform texture2D uTex2D[4];") != NULL);
    HT_CHECK(strstr(buf, "uniform sampler uSmp[5];") != NULL);
    HT_CHECK(strstr(buf, "uTex2D[(pc.tex[0] & 0xFFFFu)]") != NULL);
    dump("fixed", "frag");
    /* a cap it doesn't fit in: 0, the text as it was */
    char small[64] = "nonuniformEXT(x) uTex2D[];";
    HT_EQ_INT(hle_glsl_fixed_slots(small, 8), 0);
    HT_EQ_STR(small, "nonuniformEXT(x) uTex2D[];");
    HT_EQ_INT(hle_glsl_fixed_slots(small, sizeof(small)), strlen("(x) uTex2D[4];"));
    HT_EQ_STR(small, "(x) uTex2D[4];");
}

/* no depthClamp: a z-clamped draw's vertices clamped in the vertex shader */
static void test_zclamp(void)
{
    HleFfVsKey vk = { .fvf = 0x144, .ntex = 1, .zclamp = 1 };
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "clamp(gl_Position.z") == NULL);    /* the rasterizer's, by default */
    g_hle_glsl_zclamp = true;
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, HLE_ZCLAMP_GLSL) != NULL);
    vk.zclamp = 0;
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "clamp(gl_Position.z") == NULL);
    g_hle_glsl_zclamp = false;
}

/* transformed XYZ, no texture: diffuse only, PS SELECTARG1 DIFFUSE */
static void test_xyz_untextured(void)
{
    HleFfVsKey vk = { .fvf = 0x042 };
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "pc.mvp * vec4(iPos.xyz, 1.0)") != NULL);
    HleFfPsKey pk = { .nstages = 1 };
    pk.st[0] = (HleFfStage){ .colorop = 2, .colorarg1 = 0, .alphaop = 2, .alphaarg1 = 0,
                             .resultarg = 1 };
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "r.rgb = (vDiffuse).rgb") != NULL);
}

/* D3D8 lighting: directional + point + spot, specular, vertex diffuse as the
 * material diffuse/ambient source */
static void test_lighting(void)
{
    HleFfVsKey vk = { .fvf = 0x152, .ntex = 1, .lighting = 1, .nlights = 3,
                      .light_type = { HLE_LIGHT_DIRECTIONAL, HLE_LIGHT_POINT, HLE_LIGHT_SPOT },
                      .specular = 1, .local_viewer = 1, .normalize = 1, .colorvertex = 1,
                      .src = { 1, 1, 0, 0 } };
    HT_CHECK(hle_ff_vs_needs_ubo(&vk));
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "uniform FfConst") != NULL);
    HT_CHECK(strstr(buf, "in vec3 iNormal") != NULL);
    HT_CHECK(strstr(buf, "eN = normalize(eN)") != NULL);
    HT_CHECK(strstr(buf, "normalize(-ePos)") != NULL);       /* local viewer */
    HT_CHECK(strstr(buf, "/* light 2 */") != NULL);
    HT_CHECK(strstr(buf, "float rho") != NULL);               /* the spot cone */
    HT_CHECK(strstr(buf, "dsum * iDiffuse.rgb") != NULL);     /* diffuse from the vertex */
    HT_CHECK(strstr(buf, "ssum * f[14].rgb") != NULL);        /* material specular */
    HT_CHECK(strstr(buf, "vFog = abs(ePos.z)") != NULL);
    dump("light", "vert");
    /* without COLORVERTEX the material supplies every colour */
    vk.colorvertex = 0;
    vk.specular = 0;
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "dsum * f[12].rgb") != NULL);
    HT_CHECK(strstr(buf, "vSpecular = vec4(0.0)") != NULL);
    /* no vertex normal: lit with N.L 0, the alpha the material's diffuse
     * (Morrowind's shadow volumes, XYZ only) */
    HleFfVsKey xyz = { .fvf = 0x002, .lighting = 1, .nlights = 1,
                       .light_type = { HLE_LIGHT_DIRECTIONAL } };
    HT_CHECK(hle_ff_vs_glsl(&xyz, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "iNormal") == NULL);
    HT_CHECK(strstr(buf, "float nl = 0.0;") != NULL);
    HT_CHECK(strstr(buf, "f[12].a), 0.0, 1.0)") != NULL);
    /* a declaration's CMP normal (Morrowind's meshes): fetched as two 16-bit
     * halves, decoded before the eye-space normal */
    HleFfVsKey cmp = { .fvf = 0x112, .ntex = 1, .lighting = 1, .nlights = 1,
                       .light_type = { HLE_LIGHT_DIRECTIONAL }, .cmp_normal = 1 };
    HT_CHECK(hle_ff_vs_glsl(&cmp, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "in uvec2 iNormalP") != NULL);
    HT_CHECK(strstr(buf, "in vec3 iNormal;") == NULL);
    HT_CHECK(strstr(buf, "bitfieldExtract(nP, 22, 10)") != NULL);
    HT_CHECK(strstr(buf, "vec3 iNormal = ") < strstr(buf, "vec3 n = iNormal"));
    dump("cmpnormal", "vert");
}

/* vertex blending: 3 matrices, two weights from the vertex (VERTEXBLEND 3) */
static void test_blend(void)
{
    HleFfVsKey vk = { .fvf = 0x052, .lighting = 1, .nlights = 1,
                      .light_type = { HLE_LIGHT_DIRECTIONAL }, .blend = 3, .blend_mix = 1 };
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "in vec4 iWeight") != NULL);
    HT_CHECK(strstr(buf, "iWeight[1]") != NULL);
    HT_CHECK(strstr(buf, "iWeight[2]") == NULL);
    HT_CHECK(strstr(buf, "eP4 += wl * (m4(96) * p)") != NULL); /* world2 * view */
    dump("blend", "vert");
}

/* NGB's skinning: XYZB1, VERTEXBLEND 1 (2 matrices, w and 1 - w); and
 * VERTEXBLEND 2 (2 matrices, both weights from the vertex) */
static void test_blend_modes(void)
{
    HleFfVsKey one = { .fvf = 0x116, .lighting = 1, .nlights = 1,
                       .light_type = { HLE_LIGHT_DIRECTIONAL }, .blend = 2, .blend_mix = 1 };
    HT_CHECK(hle_ff_vs_glsl(&one, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "iWeight[0]") != NULL);
    HT_CHECK(strstr(buf, "iWeight[1]") == NULL);
    HT_CHECK(strstr(buf, "eP4 += wl * (m4(92) * p)") != NULL); /* world1 * view */
    HleFfVsKey two = one;
    two.blend_mix = 0;
    HT_CHECK(hle_ff_vs_glsl(&two, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "iWeight[1]") != NULL);
    HT_CHECK(strstr(buf, "eP4 += wl") == NULL);
}

/* texgen (reflection, sphere map, camera position) and texture transforms */
static void test_texgen(void)
{
    HleFfVsKey vk = { .fvf = 0x112, .ntex = 3, .texcoord = { 0, 0, 0 },
                      .texgen = { HLE_TCI_CAMERAREFLECTION, HLE_TCI_SPHEREMAP,
                                  HLE_TCI_CAMERAPOSITION },
                      .texxform = { 3, 0, 0x13 } };
    HT_CHECK(hle_ff_vs_needs_ubo(&vk));
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "reflect(normalize(ePos), normalize(eN))") != NULL);
    HT_CHECK(strstr(buf, "t = m4(76) * t") != NULL);          /* stage 0 matrix */
    HT_CHECK(strstr(buf, "t = m4(84) * t") != NULL);          /* stage 2 matrix */
    HT_CHECK(strstr(buf, "t = vec4(t.xyz, t.z)") != NULL);    /* COUNT3 | PROJECTED */
    dump("texgen", "vert");
    /* pre-transformed vertices ignore texgen (no eye space) */
    vk.fvf = 0x104;
    HT_CHECK(!strstr((hle_ff_vs_glsl(&vk, buf, sizeof(buf)), buf), "FfConst"));
}

/* fog: the vertex side's distance by source, the pixel side's factor and mix */
static void test_fog(void)
{
    HleFfVsKey vk = { .fvf = 0x0D2, .fog_src = HLE_FOG_SRC_RADIAL };
    HT_CHECK(hle_ff_vs_needs_ubo(&vk));
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog = length(ePos)") != NULL);
    vk.fog_src = HLE_FOG_SRC_SPEC_ALPHA;
    HT_CHECK(!hle_ff_vs_needs_ubo(&vk));
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog = iSpecular.a") != NULL);
    vk.fog_src = HLE_FOG_SRC_PLANAR;
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog = abs(ePos.z)") != NULL);
    dump("fog", "vert");
    HleFfPsKey pk = { .nstages = 1, .fog = 4 };              /* LINEAR */
    pk.st[0] = (HleFfStage){ .colorop = 2, .colorarg1 = 0, .alphaop = 2, .alphaarg1 = 0,
                             .resultarg = 1 };
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "in float vFog") != NULL);
    HT_CHECK(strstr(buf, "(pc.fog.y - vFog)") != NULL);
    HT_CHECK(strstr(buf, "mix(unpackUnorm4x8(pc.fog_color).zyx, cur.rgb, fogf)") != NULL);
    dump("fog", "frag");
    pk.fog = 0;
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vFog") == NULL);
}

/* point lists: scaled size from the block, stage 3 coordinates from the point */
static void test_points(void)
{
    HleFfVsKey vk = { .fvf = 0x142, .ntex = 4, .points = 2 };
    HT_CHECK(hle_ff_vs_needs_ubo(&vk));
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "gl_PointSize = clamp(ps, f[104].y, f[104].z) * f[105].w") != NULL);
    HT_CHECK(strstr(buf, "inversesqrt") != NULL);
    dump("points", "vert");
    vk.fvf = 0x164;                     /* XYZRHW | PSIZE | DIFFUSE | TEX1: constant size */
    vk.ntex = 1;
    HT_CHECK(hle_ff_vs_glsl(&vk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "in float iPSize") != NULL);
    HT_CHECK(strstr(buf, "inversesqrt") == NULL);
    HT_CHECK(strstr(buf, "m4(") == NULL);
    dump("points_rhw", "vert");
    /* DOA3's snow: texture stage 3 alone, colour SELECTARG1 TEXTURE, alpha
     * MODULATE TEXTURE, CURRENT */
    HleFfPsKey pk = { .nstages = 1, .point_sprite = 1 };
    pk.st[0] = (HleFfStage){ .colorop = 2, .colorarg1 = 2, .alphaop = 4, .alphaarg1 = 2,
                             .alphaarg2 = 1, .resultarg = 1, .texture = 1 };
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "uv0 = gl_PointCoord") != NULL);
    HT_CHECK(strstr(buf, "vTex0.xy") == NULL);
    dump("points", "frag");
}

/* Blinx's movie: stage 1 left at MODULATE TEXTURE, DIFFUSE with no texture
 * set; D3D's combiners pass CURRENT through there (the movie), not texture
 * white times diffuse. A texture HLE could not prepare stays white. */
static void test_unset_texture(void)
{
    HleFfPsKey pk = { .nstages = 2 };
    for (unsigned s = 0; s < 2; s++) {
        pk.st[s] = (HleFfStage){ .colorop = 4, .colorarg1 = 2, .colorarg2 = 0,
                                 .alphaop = 4, .alphaarg1 = 2, .alphaarg2 = 0,
                                 .resultarg = 1, .texture = !s, .unset = s };
    }
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "r.rgb = ((t0 * vDiffuse)).rgb") != NULL);
    HT_CHECK(strstr(buf, "r.rgb = (cur).rgb") != NULL);
    HT_CHECK(strstr(buf, "r.a = (cur).a") != NULL);
    HT_CHECK(strstr(buf, "(t1 * vDiffuse)") == NULL);
    dump("unset", "frag");
    /* the first stage passes diffuse, keeping the op's scale and bias */
    pk.nstages = 1;
    pk.st[0].texture = 0;
    pk.st[0].unset = 1;
    pk.st[0].colorop = 5;                   /* MODULATE2X */
    pk.st[0].alphaop = 9;                   /* ADDSIGNED2X */
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "r.rgb = ((2.0 * vDiffuse)).rgb") != NULL);
    HT_CHECK(strstr(buf, "r.a = ((2.0 * (vDiffuse - 0.5))).a") != NULL);
    /* an op not reading the texture is unchanged */
    pk.st[0].colorop = 2;                   /* SELECTARG1 DIFFUSE */
    pk.st[0].colorarg1 = 0;
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "r.rgb = (vDiffuse).rgb") != NULL);
    /* BLENDTEXTUREALPHA reads the texture alpha without a TEXTURE argument */
    pk.st[0].colorop = 14;
    pk.st[0].colorarg2 = 3;
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "r.rgb = (vDiffuse).rgb") != NULL);
    HT_CHECK(strstr(buf, "t0.a") == NULL);
    /* set but not prepared: the white placeholder, as before */
    pk.st[0] = (HleFfStage){ .colorop = 4, .colorarg1 = 2, .colorarg2 = 0, .alphaop = 4,
                             .alphaarg1 = 2, .alphaarg2 = 0, .resultarg = 1 };
    HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = vec4(1.0)") != NULL);
    HT_CHECK(strstr(buf, "(t0 * vDiffuse)") != NULL);
}

/* every D3DTOP and modifier combination generates (no overflow, no gaps) */
static void test_all_ops(void)
{
    for (uint32_t op = 1; op <= 26; op++) {
        for (uint32_t mod = 0; mod <= 0x30; mod += 0x10) {
            HleFfPsKey pk = { .nstages = 2 };
            for (unsigned s = 0; s < 2; s++) {
                pk.st[s] = (HleFfStage){ .colorop = op, .colorarg0 = 5 | mod,
                                         .colorarg1 = 2 | mod, .colorarg2 = 1 | mod,
                                         .alphaop = op, .alphaarg1 = 3, .alphaarg2 = 4,
                                         .resultarg = s ? 1 : 5, .texture = 1 };
            }
            HT_CHECK(hle_ff_ps_glsl(&pk, buf, sizeof(buf)) > 0);
        }
    }
}

/* a buffer too small fails instead of truncating */
static void test_overflow(void)
{
    HleFfVsKey vk = { .fvf = 0x144, .ntex = 1 };
    char small[64];
    HT_CHECK(hle_ff_vs_glsl(&vk, small, sizeof(small)) == 0);
}

int main(void)
{
    HT_RUN(test_ui_quad);
    HT_RUN(test_fixed_slots);
    HT_RUN(test_zclamp);
    HT_RUN(test_xyz_untextured);
    HT_RUN(test_lighting);
    HT_RUN(test_blend);
    HT_RUN(test_blend_modes);
    HT_RUN(test_texgen);
    HT_RUN(test_fog);
    HT_RUN(test_points);
    HT_RUN(test_unset_texture);
    HT_RUN(test_all_ops);
    HT_RUN(test_overflow);
    HT_DONE();
}
