#ifndef SCENE_H
#define SCENE_H

#include "mesh.h"
#include <stdint.h>

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 pos;
    float radius;
    float reflectivity;
    float ior;
    float roughness;
    Vec3 color;
    char material[16];
    int tex_type;
    float tex_scale;
    Vec3 tex_color2;
} Sphere;

typedef struct {
    unsigned char* data;
    int width;
    int height;
} ImageTexture;

typedef struct {
    TriGpu* tris;
    int num_tris;
    Vec3 pos;
    float scale;
    float reflectivity;
    float ior;
    float roughness;
    float metallic;
    float transmission;
    Vec3 color;
    char material[16];
    int tex_type;
    float tex_scale;
    Vec3 tex_color2;
    int tex_index;
    int orm_tex_index;
    int iri_tex_index;
    int32_t iri_color_tex_index; /* iridescenceTexture image index (factor modulation), -1 = none */
    float iri_factor;
    float iri_ior;
    float iri_thin_min;
    float iri_thin_max;
    float vol_th;
    float att_r;
    float att_g;
    float att_b;
    float att_dist;
    int vol_tex_index;
    int32_t ao_tex_index;
    int32_t nrm_tex_index;   /* normal-map image index, -1 = none */
    float nrm_scale;         /* normalTexture.scale, default 1.0 */
    int32_t alpha_mode;      /* 0 = OPAQUE, 1 = MASK */
    float alpha_cutoff;      /* alphaCutoff, default 0.5 */
    float aniso_factor;      /* KHR_materials_anisotropy strength, default 0 */
    float aniso_rotation;    /* anisotropyRotation, radians, default 0 */
    int32_t aniso_tex_index; /* anisotropyTexture image index, -1 = none */
    float cc_factor;         /* KHR_materials_clearcoat clearcoatFactor, default 0 */
    float cc_roughness;      /* clearcoatRoughnessFactor, default 0 */
    int32_t cc_nrm_tex_index;/* clearcoatNormalTexture image index, -1 = none */
} MeshObj;

typedef struct {
    Vec3 pos;
    float size;
} Light;

typedef struct {
    float duration;
    int fps;
    Vec3 orbit_center;
    float orbit_radius;
    float orbit_height;
    float orbit_bob;   /* vertical fly-over amplitude: 0 = fixed-height orbit */
} AnimationConfig;

typedef struct {
    Vec3 camera_pos;
    Vec3 camera_target;
    float aperture;
    float focus_dist;
    float fov_y;
    Light* lights;
    int num_lights;
    Sphere* spheres;
    int num_spheres;
    MeshObj* meshes;
    int num_meshes;
    int width;
    int height;
    float exposure;
    int denoise;
    float denoise_strength;
    char env_file[256];
    float env_intensity;
    int has_floor;
    int has_bg_color;
    float bg_color[3];
    char output[256];
    int has_animation;
    AnimationConfig animation;
    ImageTexture* textures;
    int num_textures;
} Scene;

#endif
