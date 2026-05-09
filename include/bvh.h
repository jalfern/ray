#ifndef BVH_H
#define BVH_H

#include "mesh.h"

#define BVH_MAX_TRIS_PER_LEAF 4
#define BVH_MAX_NODES 2097152

typedef struct {
    float bbox_min[3];
    int tri_start;
    float bbox_max[3];
    int tri_end;
    int left;
    int right;
    int _pad;
} BvhNode;

int bvh_build(BvhNode* nodes, TriGpu* tris, int num_tris);

#endif
