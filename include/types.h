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
    int mat_type;
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
