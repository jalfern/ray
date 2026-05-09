#include "obj_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define OBJ_CHUNK 1024

typedef struct { float x, y, z; } ObjV;
typedef struct { float x, y; } ObjVt;
typedef struct { float x, y, z; } ObjVn;
typedef struct { int v, vt, vn; } ObjIdx;

static ObjV* verts = NULL;
static int num_verts = 0, cap_verts = 0;
static ObjVt* texcs = NULL;
static int num_texcs = 0, cap_texcs = 0;
static ObjVn* norms = NULL;
static int num_norms = 0, cap_norms = 0;
static ObjIdx* idxs = NULL;
static int num_idx = 0, cap_idx = 0;

static void reset_state(void) {
    free(verts); verts = NULL; num_verts = 0; cap_verts = 0;
    free(texcs); texcs = NULL; num_texcs = 0; cap_texcs = 0;
    free(norms); norms = NULL; num_norms = 0; cap_norms = 0;
    free(idxs); idxs = NULL; num_idx = 0; cap_idx = 0;
}

static int gv(float x, float y, float z) {
    if (num_verts >= cap_verts) {
        cap_verts = cap_verts ? cap_verts * 2 : OBJ_CHUNK;
        verts = (ObjV*)realloc(verts, cap_verts * sizeof(ObjV));
    }
    verts[num_verts].x = x; verts[num_verts].y = y; verts[num_verts].z = z;
    return num_verts++;
}

static int gvn(float x, float y, float z) {
    if (num_norms >= cap_norms) {
        cap_norms = cap_norms ? cap_norms * 2 : OBJ_CHUNK;
        norms = (ObjVn*)realloc(norms, cap_norms * sizeof(ObjVn));
    }
    norms[num_norms].x = x; norms[num_norms].y = y; norms[num_norms].z = z;
    return num_norms++;
}

static int gi(int v, int vt, int vn) {
    if (num_idx >= cap_idx) {
        cap_idx = cap_idx ? cap_idx * 2 : OBJ_CHUNK * 3;
        idxs = (ObjIdx*)realloc(idxs, cap_idx * sizeof(ObjIdx));
    }
    idxs[num_idx].v = v; idxs[num_idx].vt = vt; idxs[num_idx].vn = vn;
    return num_idx++;
}

static ObjIdx parse_idx(const char** pp) {
    ObjIdx o = {-1, -1, -1};
    const char* p = *pp;
    o.v = atoi(p) - 1;
    while (*p && *p != '/' && !isspace(*p)) p++;
    if (*p == '/') {
        p++;
        if (*p != '/') o.vt = atoi(p) - 1;
        while (*p && *p != '/' && !isspace(*p)) p++;
        if (*p == '/') { p++; o.vn = atoi(p) - 1; }
    }
    *pp = p;
    return o;
}

int load_obj(const char* filename, TriGpu** out_tris, int* out_count) {
    FILE* f = fopen(filename, "r");
    if (!f) return -1;

    reset_state();

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        if (p[0] == 'v' && p[1] == ' ') {
            float x, y, z;
            if (sscanf(p, "v %f %f %f", &x, &y, &z) >= 3) gv(x, y, z);
        } else if (p[0] == 'v' && p[1] == 'n') {
            float x, y, z;
            if (sscanf(p, "vn %f %f %f", &x, &y, &z) >= 3) gvn(x, y, z);
        } else if (p[0] == 'f' && p[1] == ' ') {
            p += 2;
            ObjIdx fv[64];
            int nf = 0;
            while (*p && nf < 64) {
                while (*p && isspace(*p)) p++;
                if (!*p) break;
                const char* cp = p;
                fv[nf++] = parse_idx(&cp);
                p = (char*)cp;
            }
            for (int i = 1; i < nf - 1; i++) {
                gi(fv[0].v, fv[0].vt, fv[0].vn);
                gi(fv[i].v, fv[i].vt, fv[i].vn);
                gi(fv[i + 1].v, fv[i + 1].vt, fv[i + 1].vn);
            }
        }
    }
    fclose(f);

    if (num_idx == 0 || num_verts == 0 || num_idx % 3 != 0) {
        reset_state();
        return -1;
    }

    int nt = num_idx / 3;
    *out_tris = (TriGpu*)calloc(nt, sizeof(TriGpu));
    *out_count = nt;
    TriGpu* tris = *out_tris;

    for (int t = 0; t < nt; t++) {
        int i0 = idxs[t * 3].v, i1 = idxs[t * 3 + 1].v, i2 = idxs[t * 3 + 2].v;
        int n0 = idxs[t * 3].vn, n1 = idxs[t * 3 + 1].vn, n2 = idxs[t * 3 + 2].vn;

        tris[t].v0[0] = verts[i0].x; tris[t].v0[1] = verts[i0].y; tris[t].v0[2] = verts[i0].z;
        tris[t].v1[0] = verts[i1].x; tris[t].v1[1] = verts[i1].y; tris[t].v1[2] = verts[i1].z;
        tris[t].v2[0] = verts[i2].x; tris[t].v2[1] = verts[i2].y; tris[t].v2[2] = verts[i2].z;

        if (n0 >= 0 && n0 < num_norms) {
            tris[t].n0[0] = norms[n0].x; tris[t].n0[1] = norms[n0].y; tris[t].n0[2] = norms[n0].z;
            tris[t].n1[0] = norms[n1].x; tris[t].n1[1] = norms[n1].y; tris[t].n1[2] = norms[n1].z;
            tris[t].n2[0] = norms[n2].x; tris[t].n2[1] = norms[n2].y; tris[t].n2[2] = norms[n2].z;
        } else {
            float ax = verts[i1].x - verts[i0].x, ay = verts[i1].y - verts[i0].y, az = verts[i1].z - verts[i0].z;
            float bx = verts[i2].x - verts[i0].x, by = verts[i2].y - verts[i0].y, bz = verts[i2].z - verts[i0].z;
            float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            tris[t].n0[0] = nx; tris[t].n0[1] = ny; tris[t].n0[2] = nz;
            tris[t].n1[0] = nx; tris[t].n1[1] = ny; tris[t].n1[2] = nz;
            tris[t].n2[0] = nx; tris[t].n2[1] = ny; tris[t].n2[2] = nz;
        }

        tris[t].mesh_idx = 0;
    }

    reset_state();
    return nt;
}
