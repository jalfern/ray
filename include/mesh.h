#ifndef MESH_H
#define MESH_H

typedef struct {
    float v0[3], v1[3], v2[3];
    float n0[3], n1[3], n2[3];
    float t0[2], t1[2], t2[2];
    int mesh_idx;
} TriGpu;

#endif
