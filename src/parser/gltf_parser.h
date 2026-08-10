#ifndef GLTF_PARSER_H
#define GLTF_PARSER_H

#include "scene.h"

typedef struct {
    MeshObj* meshes;
    int num_meshes;
    Vec3 camera_pos;
    Vec3 camera_target;
    float aperture;
    float focus_dist;
} GltfScene;

int load_gltf(const char* path, GltfScene* out);
void free_gltf(GltfScene* out);

#endif