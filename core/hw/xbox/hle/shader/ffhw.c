/*
 * hakuX HLE GPU — the NV2A fixed-function vertex stage as GLSL (pure). See
 * ffhw.h. The transform engine's fixed function, as NV_vertex_program's
 * documentation and the hardware tests describe it (LLE's generator read as
 * reference only):
 * - skinning blends the position and normal over model-view matrices 0..n-1
 *   (and their inverses), the last weight generated (1 - the others) in the
 *   "G" modes; without skinning the composite matrix includes model-view;
 * - lighting in eye space, the registers D3D's products: light colours
 *   premultiplied by the material where its source is the material, the
 *   scene ambient (with the emission) and material emission registers
 *   reused for the vertex-colour sources;
 * - the composite matrix includes the viewport scale: x, y, z / w plus the
 *   viewport offset is the screen-space position a program leaves.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ffhw.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

/* A colour source of SET_COLOR_MATERIAL: the material's (the register
 * already holds it: 1), the diffuse or the specular register. */
static const char *src_rgb(unsigned s)
{
    return s == 1 ? "v3.rgb" : s == 2 ? "v4.rgb" : "vec3(1.0)";
}

static void skinning(Buf *b, const HleFfhwKey *k)
{
    static const struct { uint8_t n, mix; } k_skin[7] = {
        { 0, 0 }, { 2, 1 }, { 2, 0 }, { 3, 1 }, { 3, 0 }, { 4, 1 }, { 4, 0 },
    };
    unsigned n = k->skin < 7 ? k_skin[k->skin].n : 0, mix = k->skin < 7 ? k_skin[k->skin].mix : 0;
    if (!n) {
        put(b, "    tPos = XF(pos, %d);\n"
               "    tN = XF(n4, %d).xyz;\n", HLE_FFHW_C_MODELVIEW, HLE_FFHW_C_INVMODELVIEW);
        return;
    }
    put(b, "    tPos = vec4(0.0);\n"
           "    tN = vec3(0.0);\n"
           "    float wn = 1.0, wi;\n");
    for (unsigned i = 0; i < n; i++) {
        if (mix && i == n - 1) {
            put(b, "    wi = wn;\n");
        } else {
            put(b, "    wi = v1.%c;\n", "xyzw"[i]);
            if (mix) {
                put(b, "    wn -= wi;\n");
            }
        }
        put(b, "    tPos += XF(pos, %u) * wi;\n"
               "    tN += XF(n4, %u).xyz * wi;\n", HLE_FFHW_C_MODELVIEW + 4 * i,
            HLE_FFHW_C_INVMODELVIEW + 4 * i);
    }
}

static void texgen(Buf *b, const HleFfhwKey *k)
{
    for (unsigned s = 0; s < 4; s++) {
        put(b, "    oT%u = v%u;\n", s, 9 + s);
        for (unsigned j = 0; j < 4; j++) {
            const unsigned g = k->texgen[s][j], plane = HLE_FFHW_C_TEXGEN + 4 * s + j;
            const char c = "xyzw"[j];
            if (g == HLE_FFHW_TG_EYE) {
                put(b, "    oT%u.%c = dot(c[%u], tPos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_OBJECT) {
                put(b, "    oT%u.%c = dot(c[%u], pos);\n", s, c, plane);
            } else if (g == HLE_FFHW_TG_SPHERE && j < 2) {
                put(b, "    { vec3 r = reflect(normalize(tPos.xyz), tN);\n"
                       "      oT%u.%c = r.%c / (2.0 * length(r + vec3(0.0, 0.0, 1.0))) + 0.5; }\n",
                    s, c, c);
            } else if (g == HLE_FFHW_TG_REFLECTION && j < 3) {
                put(b, "    oT%u.%c = reflect(normalize(tPos.xyz), tN).%c;\n", s, c, c);
            } else if (g == HLE_FFHW_TG_NORMAL && j < 3) {
                put(b, "    oT%u.%c = tN.%c;\n", s, c, c);
            }
        }
        if (k->texmat & (1u << s)) {
            put(b, "    oT%u = XF(oT%u, %u);\n", s, s, HLE_FFHW_C_TEXMAT + 4 * s);
        }
    }
}

static void lighting(Buf *b, const HleFfhwKey *k)
{
    if (!k->lighting) {
        put(b, "    oD0 = v3;\n    oD1 = v4;\n");
        return;
    }
    const char *alpha = k->src[2] == 1 ? "v3.a" : k->src[2] == 2 ? "v4.a" : "c[%u].w";
    char a[32];
    snprintf(a, sizeof(a), alpha, HLE_FFHW_C_AMBIENT);
    /* the ambient source scaled by the emission register, plus the
     * emission source (the scene ambient register for the material: D3D
     * folds ambient * material ambient + emissive into it) */
    char amb[16];
    snprintf(amb, sizeof(amb), "c[%u].rgb", HLE_FFHW_C_AMBIENT);
    put(b, "    oD0 = vec4(%s * c[%u].rgb + %s, %s);\n",
        k->src[1] == 0 ? amb : src_rgb(k->src[1]), HLE_FFHW_C_EMISSION,
        k->src[0] == 0 ? amb : src_rgb(k->src[0]), a);
    put(b, "    oD1 = vec4(0.0, 0.0, 0.0, v4.a);\n");
    /* the direction to the eye: the eye position for a local viewer, else
     * along -z (D3D's eye space looks down +z) */
    if (k->local_eye) {
        put(b, "    vec3 eyeV = normalize(c[%u].xyz / c[%u].w - tPos.xyz / tPos.w);\n",
            HLE_FFHW_C_EYE, HLE_FFHW_C_EYE);
    } else {
        put(b, "    vec3 eyeV = vec3(0.0, 0.0, -1.0);\n");
    }
    for (unsigned i = 0; i < 8; i++) {
        const unsigned t = k->light[i], L = HLE_FFHW_C_LIGHTS + HLE_FFHW_LIGHT_VEC4 * i;
        if (!t) {
            continue;
        }
        put(b, "    { /* light %u */\n"
               "      float att = 1.0, nl, nh;\n"
               "      bool lit = true;\n", i);
        if (t == 1) {
            put(b, "      vec3 Ld = normalize(c[%u].xyz);\n"
                   "      nl = max(dot(tN, Ld), 0.0);\n", L + HLE_FFHW_L_DIRECTION);
            if (k->local_eye) {
                put(b, "      nh = max(dot(tN, normalize(Ld + eyeV)), 0.0);\n");
            } else {
                put(b, "      nh = max(dot(tN, c[%u].xyz), 0.0);\n", L + HLE_FFHW_L_HALF);
            }
        } else {
            put(b, "      vec3 VP = c[%u].xyz - tPos.xyz / tPos.w;\n"
                   "      float d = length(VP);\n"
                   "      lit = d <= c[%u].x;\n"
                   "      VP = normalize(VP);\n"
                   "      vec3 at = c[%u].xyz;\n"
                   "      att = 1.0 / (at.x + at.y * d + at.z * d * d);\n"
                   "      nl = max(dot(tN, VP), 0.0);\n"
                   "      nh = max(dot(tN, normalize(VP + eyeV)), 0.0);\n",
                L + HLE_FFHW_L_POSITION, L + HLE_FFHW_L_RANGE, L + HLE_FFHW_L_ATTENUATION);
            if (t == 3) {
                /* D3D's spot: the direction scaled by 1 / (cos(theta/2) -
                 * cos(phi/2)), w = -cos(phi/2) times that: rho past the
                 * outer cone 0, inside the inner one 1, between linear */
                put(b, "      vec4 sd = c[%u];\n"
                       "      float inv = 1.0 / length(sd.xyz), cphi = -inv * sd.w;\n"
                       "      float sdv = dot(sd.xyz, VP), rho = inv * sdv;\n"
                       "      if (rho <= cphi) att = 0.0;\n"
                       "      else if (rho <= inv + cphi) att *= sdv + sd.w;\n",
                    L + HLE_FFHW_L_SPOT);
            }
        }
        put(b, "      if (lit) {\n"
               "        float pf = nl == 0.0 || nh == 0.0 ? 0.0 : pow(nh, c[%u].x);\n"
               "        oD0.rgb += c[%u].rgb * att;\n"
               "        oD0.rgb += %s * c[%u].rgb * (att * nl);\n"
               "        oD1.rgb += %s * c[%u].rgb * (att * pf);\n"
               "      }\n"
               "    }\n",
            HLE_FFHW_C_PARAMS, L + HLE_FFHW_L_AMBIENT, src_rgb(k->src[2]), L + HLE_FFHW_L_DIFFUSE,
            src_rgb(k->src[3]), L + HLE_FFHW_L_SPECULAR);
    }
}

size_t hle_ffhw_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *vk, char *buf,
                     size_t cap)
{
    if (!hle_ffhw_is(tokens, nslots)) {
        return 0;
    }
    HleFfhwKey k;
    memcpy(&k, tokens, sizeof(k));
    char body[24576];
    Buf b = { body, sizeof(body), 0, false };
    put(&b, "#define XF(v, b) vec4(dot((v), c[(b)]), dot((v), c[(b) + 1]), "
            "dot((v), c[(b) + 2]), dot((v), c[(b) + 3]))\n"
            "  {\n"
            "    vec4 pos = v0, n4 = vec4(v2.xyz, 0.0), tPos;\n"
            "    vec3 tN;\n");
    skinning(&b, &k);
    if (k.normalize) {
        put(&b, "    tN = normalize(tN);\n");
    }
    texgen(&b, &k);
    lighting(&b, &k);
    put(&b, "    oB0 = v7;\n    oB1 = v8;\n");
    if (!k.specular) {
        put(&b, "    oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n    oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        if (!k.separate_spec) {
            if (k.lighting) {
                put(&b, "    oD0.rgb += oD1.rgb;\n");
            }
            put(&b, "    oD1 = v4;\n");
        }
        if (!k.alpha_mat_spec) {
            put(&b, "    oD1.a = 1.0;\n    oB1.a = 1.0;\n");
        }
    }
    if (k.fog) {
        /* the fog coordinate by SET_FOG_GEN_MODE */
        switch (k.fog - 1) {
        case 0: put(&b, "    oFog = vec4(clamp(v4.a, 0.0, 1.0));\n"); break;
        case 1: put(&b, "    oFog = vec4(length(tPos.xyz));\n"); break;
        case 2: put(&b, "    oFog = vec4(dot(c[%u].xyz, tPos.xyz) + c[%u].w);\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        case 3: put(&b, "    oFog = vec4(abs(dot(c[%u].xyz, tPos.xyz) + c[%u].w));\n",
                    HLE_FFHW_C_FOGPLANE, HLE_FFHW_C_FOGPLANE); break;
        default: put(&b, "    oFog = vec4(v5.x);\n"); break;
        }
    }
    if (k.points) {
        const unsigned p = HLE_FFHW_C_POINT;
        put(&b, "    { float de = length(tPos.xyz);\n"
                "      float pmin = min(c[%u].w, 63.875), pmax = min(c[%u].w + pmin, 63.875);\n"
                "      oPts.x = clamp((inversesqrt(c[%u].x + c[%u].y * de + c[%u].z * de * de) + "
                "c[%u].z) * c[%u].w + c[%u].w, pmin, pmax); }\n",
            p + 1, p, p, p, p, p + 1, p, p + 1);
    } else {
        put(&b, "    oPts.x = max(c[%u].z, 1.0);\n", HLE_FFHW_C_PARAMS);
    }
    /* screen space: the composite (viewport scale included) over w, plus the
     * viewport offset; w kept for the clip space (as RCC, its reciprocal's
     * magnitude clamped) */
    put(&b, "    vec4 sp = XF(%s, %u);\n"
            "    float aw = clamp(abs(sp.w), 5.421011e-20, 1.8446744e19);\n"
            "    float sw = sp.w < 0.0 ? -aw : aw;\n"
            "    oPos = vec4(sp.xyz / sw + c[%u].xyz, sw);\n"
            "  }\n"
            "#undef XF\n",
        k.skin ? "tPos" : "pos", HLE_FFHW_C_COMPOSITE, HLE_FFHW_C_VPOFF);
    if (b.overflow) {
        return 0;
    }
    return hle_vsh_glsl_wrap(vk, body, k.fog != 0, buf, cap);
}

/* ---- the specular exponent ---- */

/* D3D turns the material's power into six coefficients of the hardware's
 * specular curve; the exponent comes back from the first (and the fourth,
 * the half power's) by piecewise fits over the powers D3D produces (the
 * curve fit of the hardware tests, LLE's pgraph_kelvin_map.c, read as
 * reference). */
static float fit(float c0, unsigned i)
{
    static const struct { double a, b, c; uint8_t ratio; } k_fit[9] = {
        { 1.000108475163, -9.838607076280, 54.829089549713, 0 },
        { 1.199164441703, -3.292603784852, 7.799987995214, 0 },
        { 8.653441252033, 29.189473787191, 43.586027561823, 0 },
        { -531.307758450301, 117.398468683934, 113.155490738338, 1 },
        { -4.662713151292, 1.221108944572, 1.217360986939, 1 },
        { -124.435242105211, 35.401219563514, 35.408114377045, 1 },
        { 10672560.259502287954, 21565843.555823743343, 10894794.336297152564, 0 },
        { -51973801.463933646679, -104199997.554352939129, -52225454.356278456748, 0 },
        { 972270.324080004124, 2025882.096547174733, 1054898.052467488218, 0 },
    };
    const double x = c0;
    return (float)(k_fit[i].ratio ? k_fit[i].a * x / (k_fit[i].b + k_fit[i].c * x)
                                  : k_fit[i].a + k_fit[i].b * x + k_fit[i].c * x * x);
}

static float power_of(float c0)
{
    /* where each fit starts: the coefficient at powers 1.25, 4, 9, 20, 90,
     * 375, 650, 1000, 1250 */
    static const float k_step[9] = {
        -0.022553957999f, -0.421539008617f, -0.678715527058f, -0.838916420937f,
        -0.961754500866f, -0.990773200989f, -0.994858562946f, -0.996561050415f,
        -0.999547004700f,
    };
    for (unsigned i = 0; i < 9; i++) {
        if (c0 > k_step[i]) {
            return fit(c0, i);
        }
    }
    return 0.0f;
}

float hle_ffhw_specular_power(const float p[6])
{
    if (p[1] > 0.0f && p[2] < 1.0f) {
        return p[2] - 2.0f * p[0];      /* powers below 1 */
    }
    if (p[0] > 0.0f || p[3] > 0.0f) {
        return 0.0001f;                 /* not D3D's */
    }
    const float pw = power_of(p[0]), half = power_of(p[3]);
    /* past 1250 the half power's coefficients carry it */
    if (pw == 0.0f || (half > pw && p[0] < -0.1f)) {
        return 2.0f * half;
    }
    return pw;
}
