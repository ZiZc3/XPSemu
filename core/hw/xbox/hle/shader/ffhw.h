/*
 * hakuX HLE GPU — the NV2A fixed-function vertex stage as GLSL (pure), for
 * push-buffer mode's draws in Kelvin's fixed-function mode
 * (SET_TRANSFORM_EXECUTION_MODE 0). The state is the hardware's as the
 * methods set it, not D3D's (shader/ff_gen.c models D3D's): D3D
 * premultiplies lights by the material, folds the viewport into the
 * composite matrix and hands the lights over in eye space.
 *
 * The shader is a vertex program's (vsh.h hle_vsh_glsl_wrap): inputs v0..v15
 * by attribute register (0 position, 1 weight, 2 normal, 3 diffuse, 4
 * specular, 5 fog, 6 point size, 7 / 8 back colours, 9..12 texture
 * coordinates), the constants c[] in the HLE_FFHW_C_* layout below, oPos
 * left in screen space. The back end takes it through the program path: the
 * key travels as a program's tokens (HleCmdVsh), marked by its magic.
 */
#ifndef HW_XBOX_HLE_SHADER_FFHW_H
#define HW_XBOX_HLE_SHADER_FFHW_H

#include <stddef.h>
#include <stdint.h>
#include "hw/xbox/hle/shader/vsh.h"

#define HLE_FFHW_MAGIC 0x31574846u      /* "FHW1": never a program's first word (0) */

/* texture coordinate generation (SET_TEXGEN_S/T/R/Q) */
enum { HLE_FFHW_TG_OFF, HLE_FFHW_TG_EYE, HLE_FFHW_TG_OBJECT, HLE_FFHW_TG_SPHERE,
       HLE_FFHW_TG_REFLECTION, HLE_FFHW_TG_NORMAL };

typedef struct HleFfhwKey {
    uint32_t magic;
    uint8_t skin;                       /* SET_SKIN_MODE: 0 off, 1 2G .. 6 4 */
    uint8_t lighting;                   /* SET_LIGHTING_ENABLE */
    uint8_t normalize;                  /* SET_NORMALIZATION_ENABLE */
    uint8_t local_eye;                  /* SET_LIGHT_CONTROL: LOCALEYE */
    uint8_t separate_spec;              /* ... SEPARATE_SPECULAR */
    uint8_t alpha_mat_spec;             /* ... ALPHA_FROM_MATERIAL_SPECULAR */
    uint8_t specular;                   /* SET_SPECULAR_ENABLE */
    uint8_t fog;                        /* SET_FOG_ENABLE: SET_FOG_GEN_MODE + 1; 0 off */
    uint8_t src[4];                     /* SET_COLOR_MATERIAL: emission, ambient, diffuse,
                                           specular from 0 the material, 1 the diffuse
                                           (v3), 2 the specular (v4) */
    uint8_t light[8];                   /* SET_LIGHT_ENABLE_MASK: 0 off, 1 infinite, 2 local,
                                           3 spot */
    uint8_t texgen[4][4];               /* stage, S T R Q: HLE_FFHW_TG_* */
    uint8_t texmat;                     /* SET_TEXTURE_MATRIX_ENABLE, a bit per stage */
    uint8_t points;                     /* SET_POINT_PARAMS_ENABLE */
    uint8_t pad[6];
} HleFfhwKey;                           /* 48 bytes: three program slots */

#define HLE_FFHW_SLOTS (sizeof(HleFfhwKey) / 16)

/* The constants (vec4 slots of c[]); matrices are the methods' four rows,
 * each a dot product (as the transform engine applies them). */
enum {
    HLE_FFHW_C_COMPOSITE = 0,           /* SET_COMPOSITE_MATRIX */
    HLE_FFHW_C_MODELVIEW = 4,           /* SET_MODEL_VIEW_MATRIX 0..3 */
    HLE_FFHW_C_INVMODELVIEW = 20,       /* SET_INVERSE_MODEL_VIEW_MATRIX 0..3 */
    HLE_FFHW_C_TEXMAT = 36,             /* SET_TEXTURE_MATRIX 0..3 */
    HLE_FFHW_C_TEXGEN = 52,             /* SET_TEXGEN_PLANE_S/T/R/Q of stages 0..3 */
    HLE_FFHW_C_VPOFF = 68,              /* SET_VIEWPORT_OFFSET */
    HLE_FFHW_C_FOGPLANE = 69,           /* SET_FOG_PLANE */
    HLE_FFHW_C_EYE = 70,                /* SET_EYE_POSITION */
    HLE_FFHW_C_AMBIENT = 71,            /* SET_SCENE_AMBIENT_COLOR, SET_MATERIAL_ALPHA */
    HLE_FFHW_C_EMISSION = 72,           /* SET_MATERIAL_EMISSION */
    HLE_FFHW_C_PARAMS = 73,             /* specular power (front, back), point size */
    HLE_FFHW_C_POINT = 74,              /* SET_POINT_PARAMS (8) */
    HLE_FFHW_C_LIGHTS = 76,             /* per light HLE_FFHW_LIGHT_VEC4: */
    HLE_FFHW_COUNT = 156,
};
enum {
    HLE_FFHW_L_AMBIENT, HLE_FFHW_L_DIFFUSE, HLE_FFHW_L_SPECULAR,
    HLE_FFHW_L_RANGE,                   /* SET_LIGHT_LOCAL_RANGE (x) */
    HLE_FFHW_L_HALF,                    /* SET_LIGHT_INFINITE_HALF_VECTOR */
    HLE_FFHW_L_DIRECTION,               /* SET_LIGHT_INFINITE_DIRECTION */
    HLE_FFHW_L_FALLOFF,                 /* SET_LIGHT_SPOT_FALLOFF */
    HLE_FFHW_L_SPOT,                    /* SET_LIGHT_SPOT_DIRECTION */
    HLE_FFHW_L_POSITION,                /* SET_LIGHT_LOCAL_POSITION */
    HLE_FFHW_L_ATTENUATION,             /* SET_LIGHT_LOCAL_ATTENUATION */
    HLE_FFHW_LIGHT_VEC4,
};

/* The shader for key tokens (a program's slots holding an HleFfhwKey);
 * 0 when they don't hold one or the text does not fit. */
size_t hle_ffhw_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *vk, char *buf,
                     size_t cap);

/* Is a program's tokens an HleFfhwKey? */
static inline int hle_ffhw_is(const uint32_t *tokens, unsigned nslots)
{
    return tokens && nslots >= HLE_FFHW_SLOTS && tokens[0] == HLE_FFHW_MAGIC;
}

/* The specular exponent SET_SPECULAR_PARAMS's six coefficients stand for
 * (D3D derives them from the material's power). */
float hle_ffhw_specular_power(const float p[6]);

#endif
