#ifndef MESH_H
#define MESH_H

typedef struct {
    float v0[3], v1[3], v2[3];
    float n0[3], n1[3], n2[3];
    int mesh_idx;
} TriGpu;

#endif
