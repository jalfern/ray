#ifndef GLTF_DEBUG_H
#define GLTF_DEBUG_H

#include "scene.h"
#include "gltf_parser_internal.h"
#include "../src/parser/gltf_parser.h"

#include "scene.h"
#include "bvh.h"

extern int g_gltf_debug_enabled;
extern int g_tri_debug;

void gltf_debug_print(
    const GltfBuffer* bufs, int nb,
    const GltfBufferView* views, int nv,
    const GltfAccessor* accs, int na,
    const GltfMeshRef* mesh_refs, int nm,
    const GltfMeshData* meshes,
    const char* gltf_path);

void gltf_debug_global_arrays(
    const Scene* scene,
    const TriGpu* all_tris, int total_tris,
    const int* tri_offset,
    const BvhNode* all_bvh, int num_bvh_nodes,
    int pass);

#endif