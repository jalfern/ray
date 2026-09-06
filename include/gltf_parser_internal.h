#ifndef GLTF_PARSER_INTERNAL_H
#define GLTF_PARSER_INTERNAL_H

#include "scene.h"

#define MAX_BUFFERS 16
#define MAX_VIEWS   64
#define MAX_ACCESSORS 256
#define MAX_MESHES   256
#define MAX_PRIMITIVES 256
#define MAX_NODES    128
#define MAX_MATERIALS 32

typedef struct {
    unsigned char* data;
    int byte_length;
} GltfBuffer;

typedef struct {
    int buffer;
    int byte_offset;
    int byte_length;
    int byte_stride;
} GltfBufferView;

typedef struct {
    int buffer_view;
    int byte_offset;
    int component_type;
    int count;
    int num_components;
} GltfAccessor;

typedef struct {
    float* positions;
    float* normals;
    float* texcoords;
    int*   indices;
    float* tangents;   /* per face-vertex tangent xyz + handedness (num_indices*4),
                        or NULL. From the TANGENT attribute if present, else
                        generated with MikkTSpace. */
    int num_verts;
    int num_indices;
    int material;
} GltfPrimitiveData;

typedef struct {
    GltfPrimitiveData prims[MAX_PRIMITIVES];
    int num_prims;
} GltfMeshData;

typedef struct {
    int pos_acc;
    int norm_acc;
    int tex_acc;
    int tan_acc;
    int idx_acc;
    int material;
} GltfPrimitiveRef;

typedef struct {
    GltfPrimitiveRef prims[MAX_PRIMITIVES];
    int num_prims;
} GltfMeshRef;

#endif