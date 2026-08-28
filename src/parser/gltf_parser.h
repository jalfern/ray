#ifndef GLTF_PARSER_H
#define GLTF_PARSER_H

#include "scene.h"

typedef struct {
    unsigned char* data;
    int width;
    int height;
} GltfTexture;

typedef struct {
    MeshObj* meshes;
    int num_meshes;
    Vec3 camera_pos;
    Vec3 camera_target;
    float aperture;
    float focus_dist;
    float fov_y;
    GltfTexture* textures;
    int num_textures;
} GltfScene;

extern int g_gltf_debug_enabled;
extern int g_tri_debug;

int load_gltf(const char* path, GltfScene* out);
void free_gltf(GltfScene* out);

#endif