#ifndef TYPES_H
#define TYPES_H

#include "vector.h"
#include "scene.h"
#include "mesh.h"
#include "bvh.h"

#define MAT_GLASS 0
#define MAT_PLASTIC 1
#define MAT_EMISSIVE 2
#define MAT_METALLIC 3
#define MAT_SUBSURFACE 4

#define TEX_NONE 0
#define TEX_CHECKER 1
#define TEX_POLKA 2
#define TEX_MARBLE 3
#define TEX_RINGS 4

typedef struct {
    int type;
    float scale;
    V color2;
} TextureData;

typedef struct {
    V c;
    float r;
    float ref;
    float ior;
    float roughness;
    V col;
    int mat_type;
    TextureData tex;
} SphereData;

typedef struct {
    TriGpu* tris;
    int num_tris;
    BvhNode* bvh_nodes;
    int num_bvh_nodes;
    V col;
    float ref;
    float ior;
    float roughness;
    float metallic;
    float transmission;
    int mat_type;
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
    float cc_nrm_scale;      /* clearcoatNormalTexture.scale, default 1.0 */
    V pos;
    float scale;
    TextureData tex;
} MeshObjData;

typedef struct {
    V pos;
    float size;
} LightData;

typedef struct {
    V emitted;
    float area;
    int type;       // 0 = sphere, 1 = mesh
    int src_idx;    // index into spheres[] or meshes[]
    V c;            // sphere center
    float r;        // sphere radius
    TriGpu* tris;   // mesh triangles
    int num_tris;
    float* tri_cdf; // cumulative area per triangle (size num_tris+1)
    float total_area;
    BvhNode* bvh_nodes;
    int num_bvh_nodes;
} EmissiveSurf;

#endif
