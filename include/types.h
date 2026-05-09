#ifndef TYPES_H
#define TYPES_H

#include "vector.h"
#include "scene.h"
#include "mesh.h"

typedef struct {
    V c;
    float r;
    float ref;
    float ior;
    V col;
    int mat_type;
} SphereData;

typedef struct {
    TriGpu* tris;
    int num_tris;
    V col;
    float ref;
    float ior;
    int mat_type;
    V pos;
    float scale;
} MeshObjData;

#endif
