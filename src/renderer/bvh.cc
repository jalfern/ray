#include "bvh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#define SAH_BINS 12

static float bbox_area(const float* min, const float* max) {
    float dx = max[0] - min[0];
    float dy = max[1] - min[1];
    float dz = max[2] - min[2];
    if (dx < 0 || dy < 0 || dz < 0) return 0.0f;
    return 2.0f * (dx * dy + dx * dz + dy * dz);
}

struct SahBin {
    int count;
    float bmin[3], bmax[3];
    void init() { count = 0; bmin[0]=bmin[1]=bmin[2]=1e9f; bmax[0]=bmax[1]=bmax[2]=-1e9f; }
    void add(float mn[3], float mx[3]) {
        count++;
        for (int k = 0; k < 3; k++) {
            if (mn[k] < bmin[k]) bmin[k] = mn[k];
            if (mx[k] > bmax[k]) bmax[k] = mx[k];
        }
    }
};

static void tri_bbox(const TriGpu* tris, int idx, float* min, float* max) {
    const float* v[3] = {tris[idx].v0, tris[idx].v1, tris[idx].v2};
    min[0] = fminf(fminf(v[0][0], v[1][0]), v[2][0]);
    min[1] = fminf(fminf(v[0][1], v[1][1]), v[2][1]);
    min[2] = fminf(fminf(v[0][2], v[1][2]), v[2][2]);
    max[0] = fmaxf(fmaxf(v[0][0], v[1][0]), v[2][0]);
    max[1] = fmaxf(fmaxf(v[0][1], v[1][1]), v[2][1]);
    max[2] = fmaxf(fmaxf(v[0][2], v[1][2]), v[2][2]);
}

static void centroid(const TriGpu* tris, int idx, float* c) {
    c[0] = (tris[idx].v0[0] + tris[idx].v1[0] + tris[idx].v2[0]) / 3.0f;
    c[1] = (tris[idx].v0[1] + tris[idx].v1[1] + tris[idx].v2[1]) / 3.0f;
    c[2] = (tris[idx].v0[2] + tris[idx].v1[2] + tris[idx].v2[2]) / 3.0f;
}

static void bbox_union(const float* min_a, const float* max_a,
                        const float* min_b, const float* max_b,
                        float* min_out, float* max_out) {
    min_out[0] = fminf(min_a[0], min_b[0]);
    min_out[1] = fminf(min_a[1], min_b[1]);
    min_out[2] = fminf(min_a[2], min_b[2]);
    max_out[0] = fmaxf(max_a[0], max_b[0]);
    max_out[1] = fmaxf(max_a[1], max_b[1]);
    max_out[2] = fmaxf(max_a[2], max_b[2]);
}

struct TriRef {
    int idx;
    float c[3];
};

static int build_rec(BvhNode* nodes, int& node_count,
                     TriGpu* tris, TriRef* refs, int start, int end) {
    int node_idx = node_count++;

    float bmin[3], bmax[3];
    tri_bbox(tris, refs[start].idx, bmin, bmax);
    for (int i = start + 1; i < end; i++) {
        float mn[3], mx[3];
        tri_bbox(tris, refs[i].idx, mn, mx);
        bbox_union(bmin, bmax, mn, mx, bmin, bmax);
    }
    nodes[node_idx].bbox_min[0] = bmin[0];
    nodes[node_idx].bbox_min[1] = bmin[1];
    nodes[node_idx].bbox_min[2] = bmin[2];
    nodes[node_idx].bbox_max[0] = bmax[0];
    nodes[node_idx].bbox_max[1] = bmax[1];
    nodes[node_idx].bbox_max[2] = bmax[2];

    int count = end - start;
    if (count <= BVH_MAX_TRIS_PER_LEAF) {
        nodes[node_idx].tri_start = start;
        nodes[node_idx].tri_end = end;
        nodes[node_idx].left = -1;
        nodes[node_idx].right = -1;
        return node_idx;
    }

    // Find longest axis
    float diag[3] = {bmax[0] - bmin[0], bmax[1] - bmin[1], bmax[2] - bmin[2]};
    int axis = 0;
    if (diag[1] > diag[0]) axis = 1;
    if (diag[2] > diag[axis]) axis = 2;

    float parent_area = bbox_area(bmin, bmax);

    // Find centroid range along the chosen axis
    float cmin = refs[start].c[axis], cmax = refs[start].c[axis];
    for (int i = start + 1; i < end; i++) {
        if (refs[i].c[axis] < cmin) cmin = refs[i].c[axis];
        if (refs[i].c[axis] > cmax) cmax = refs[i].c[axis];
    }

    int best_split = -1;
    float best_cost = 1e9f;

    if (cmax > cmin && parent_area > 0) {
        // SAH binning
        float cscale = SAH_BINS / (cmax - cmin);
        SahBin bins[SAH_BINS];
        for (int b = 0; b < SAH_BINS; b++) bins[b].init();

        for (int i = start; i < end; i++) {
            int b = (int)((refs[i].c[axis] - cmin) * cscale);
            if (b < 0) b = 0;
            if (b >= SAH_BINS) b = SAH_BINS - 1;
            float mn[3], mx[3];
            tri_bbox(tris, refs[i].idx, mn, mx);
            bins[b].add(mn, mx);
        }

        // Left-to-right cumulative sweep
        float lmin[SAH_BINS-1][3], lmax[SAH_BINS-1][3];
        int lcnt[SAH_BINS-1];
        {
            float cur_min[3] = {1e9f,1e9f,1e9f}, cur_max[3] = {-1e9f,-1e9f,-1e9f};
            int cur_cnt = 0;
            for (int b = 0; b < SAH_BINS-1; b++) {
                cur_cnt += bins[b].count;
                for (int k = 0; k < 3; k++) {
                    if (bins[b].bmin[k] < cur_min[k]) cur_min[k] = bins[b].bmin[k];
                    if (bins[b].bmax[k] > cur_max[k]) cur_max[k] = bins[b].bmax[k];
                }
                lcnt[b] = cur_cnt;
                lmin[b][0]=cur_min[0]; lmin[b][1]=cur_min[1]; lmin[b][2]=cur_min[2];
                lmax[b][0]=cur_max[0]; lmax[b][1]=cur_max[1]; lmax[b][2]=cur_max[2];
            }
        }

        // Right-to-left cumulative sweep
        float rmin[SAH_BINS-1][3], rmax[SAH_BINS-1][3];
        int rcnt[SAH_BINS-1];
        {
            float cur_min[3] = {1e9f,1e9f,1e9f}, cur_max[3] = {-1e9f,-1e9f,-1e9f};
            int cur_cnt = 0;
            for (int b = SAH_BINS-1; b > 0; b--) {
                cur_cnt += bins[b].count;
                for (int k = 0; k < 3; k++) {
                    if (bins[b].bmin[k] < cur_min[k]) cur_min[k] = bins[b].bmin[k];
                    if (bins[b].bmax[k] > cur_max[k]) cur_max[k] = bins[b].bmax[k];
                }
                int sp = b - 1;
                rcnt[sp] = cur_cnt;
                rmin[sp][0]=cur_min[0]; rmin[sp][1]=cur_min[1]; rmin[sp][2]=cur_min[2];
                rmax[sp][0]=cur_max[0]; rmax[sp][1]=cur_max[1]; rmax[sp][2]=cur_max[2];
            }
        }

        // Evaluate SAH cost at each split
        for (int b = 0; b < SAH_BINS-1; b++) {
            if (lcnt[b] == 0 || rcnt[b] == 0) continue;
            float left_a = bbox_area(lmin[b], lmax[b]);
            float right_a = bbox_area(rmin[b], rmax[b]);
            float cost = (left_a * lcnt[b] + right_a * rcnt[b]) / parent_area;
            if (cost < best_cost) { best_cost = cost; best_split = b; }
        }
    }

    int mid;
    if (best_split >= 0 && best_cost < count) {
        // SAH split: partition refs by bin
        float split = cmin + (best_split + 1) * (cmax - cmin) / SAH_BINS;
        int left = start, right = end - 1;
        while (left <= right) {
            if (refs[left].c[axis] < split) { left++; continue; }
            if (refs[right].c[axis] >= split) { right--; continue; }
            TriRef tmp = refs[left]; refs[left] = refs[right]; refs[right] = tmp;
            left++; right--;
        }
        mid = left;
    } else {
        // Fallback: median split with sort
        std::sort(refs + start, refs + end, [axis](const TriRef& a, const TriRef& b) {
            return a.c[axis] < b.c[axis];
        });
        mid = (start + end) / 2;
    }

    nodes[node_idx].tri_start = -1;
    nodes[node_idx].tri_end = -1;
    nodes[node_idx].left = build_rec(nodes, node_count, tris, refs, start, mid);
    nodes[node_idx].right = build_rec(nodes, node_count, tris, refs, mid, end);
    return node_idx;
}

int bvh_build(BvhNode* nodes, TriGpu* tris, int num_tris) {
    if (num_tris == 0) return 0;

    TriRef* refs = (TriRef*)malloc(num_tris * sizeof(TriRef));
    for (int i = 0; i < num_tris; i++) {
        refs[i].idx = i;
        centroid(tris, i, refs[i].c);
    }

    int node_count = 0;
    build_rec(nodes, node_count, tris, refs, 0, num_tris);

    // Reorder tris so leaf ranges are contiguous
    TriGpu* reordered = (TriGpu*)malloc(num_tris * sizeof(TriGpu));
    int* remap = (int*)malloc(num_tris * sizeof(int));
    int out_idx = 0;

    // Flatten leaf ranges
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].left == -1) {
            for (int j = nodes[i].tri_start; j < nodes[i].tri_end; j++) {
                remap[refs[j].idx] = out_idx;
                reordered[out_idx] = tris[refs[j].idx];
                out_idx++;
            }
        }
    }

    // Update leaf ranges to new indices
    out_idx = 0;
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].left == -1) {
            int old_start = nodes[i].tri_start;
            nodes[i].tri_start = out_idx;
            out_idx += (nodes[i].tri_end - old_start);
            nodes[i].tri_end = out_idx;
        }
    }

    memcpy(tris, reordered, num_tris * sizeof(TriGpu));
    free(reordered);
    free(remap);
    free(refs);

    return node_count;
}
