/* Phase 3 Stage 1 sanity check: MikkTSpace tangent generation.
 *
 * The adapter callbacks below are a verbatim mirror of the TanGen
 * adapter in src/parser/gltf_parser.cc (same convention as the
 * shaders.metal MSL mirror — keep them in sync).  This tool exists
 * because the test asset carries no TANGENT attribute, so MikkTSpace
 * generation is the live path, and a convention error (face/vert
 * index swap, texcoord stride, handedness sign) must be caught here,
 * not in Stage 3 where it hides among shader terms.
 *
 * Test mesh: unit square in XY, z=0, normal (0,0,1), UVs u along +x
 * and v along +y, split into two triangles sharing edge v1-v2.
 * Hand-computed answer: T = dP/du = (1,0,0) at every vertex, and
 * since cross(N,T) = (0,0,1)x(1,0,0) = (0,1,0) = dP/dv, the frame
 * is orientation-preserving: handedness +1.  A wrong face/vert map
 * or texcoord stride garbles the UVs and moves T off (1,0,0); a
 * flipped handedness convention flips the sign column.
 *
 * Build (Makefile: `make tanchk`):
 *   cc -O2 -std=c11 -I./third_party/mikktspace tools/tan_check.c \
 *      third_party/mikktspace/mikktspace.c -o build/tools/tan_check -lm
 * Exit code 0 = PASS.
 */

#include <stdio.h>
#include <math.h>
#include "mikktspace.h"

typedef struct {
    const float* pos;
    const float* nrm;
    const float* tex;
    const int*   idx;
    int num_tris;
    float* out_tan;   /* num_verts * 4 floats */
} TanGen;

static int tg_num_faces(const SMikkTSpaceContext* c) {
    return ((const TanGen*)c->m_pUserData)->num_tris;
}

static int tg_num_verts_of_face(const SMikkTSpaceContext* c, const int f) {
    (void)c; (void)f;
    return 3;
}

static void tg_get_pos(const SMikkTSpaceContext* c, float out[3], const int f, const int v) {
    const TanGen* t = (const TanGen*)c->m_pUserData;
    const float* p = t->pos + (size_t)t->idx[f * 3 + v] * 3;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
}

static void tg_get_nrm(const SMikkTSpaceContext* c, float out[3], const int f, const int v) {
    const TanGen* t = (const TanGen*)c->m_pUserData;
    const float* p = t->nrm + (size_t)t->idx[f * 3 + v] * 3;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
}

static void tg_get_tex(const SMikkTSpaceContext* c, float out[2], const int f, const int v) {
    const TanGen* t = (const TanGen*)c->m_pUserData;
    const float* p = t->tex + (size_t)t->idx[f * 3 + v] * 2;
    out[0] = p[0]; out[1] = p[1];
}

static void tg_set_tspace(const SMikkTSpaceContext* c, const float tang[3], const float fSign,
                          const int f, const int v) {
    TanGen* t = (TanGen*)c->m_pUserData;
    float* o = t->out_tan + (size_t)t->idx[f * 3 + v] * 4;
    o[0] = tang[0]; o[1] = tang[1]; o[2] = tang[2]; o[3] = fSign;
}

int main(void) {
    /* Unit square, CCW split by diagonal v1-v2. */
    float pos[12] = { 0,0,0,  1,0,0,  0,1,0,  1,1,0 };
    float nrm[12] = { 0,0,1,  0,0,1,  0,0,1,  0,0,1 };
    float tex[8]  = { 0,0,    1,0,    0,1,    1,1 };
    int idx[6]    = { 0,1,2,  1,3,2 };
    float out[16];
    TanGen tg;
    SMikkTSpaceInterface iface;
    SMikkTSpaceContext ctx;
    int i, fails = 0;

    for (i = 0; i < 16; i++) out[i] = 0.0f;
    tg.pos = pos; tg.nrm = nrm; tg.tex = tex; tg.idx = idx;
    tg.num_tris = 2;
    tg.out_tan = out;
    iface.m_getNumFaces = tg_num_faces;
    iface.m_getNumVerticesOfFace = tg_num_verts_of_face;
    iface.m_getPosition = tg_get_pos;
    iface.m_getNormal = tg_get_nrm;
    iface.m_getTexCoord = tg_get_tex;
    iface.m_setTSpace = 0;
    iface.m_setTSpaceBasic = tg_set_tspace;
    ctx.m_pInterface = &iface;
    ctx.m_pUserData = &tg;

    if (!genTangSpaceDefault(&ctx)) {
        printf("FAIL: genTangSpaceDefault returned false\n");
        return 1;
    }

    printf("per-vertex tangents (expect 1,0,0,+/-1 per hand calc):\n");
    for (i = 0; i < 4; i++)
        printf("  v%d: T=(%.6f, %.6f, %.6f) w=%+.3f\n",
               i, out[i*4], out[i*4+1], out[i*4+2], out[i*4+3]);

    for (i = 0; i < 4; i++) {
        float tx = out[i*4], ty = out[i*4+1], tz = out[i*4+2], w = out[i*4+3];
        float len2 = tx*tx + ty*ty + tz*tz;
        if (fabsf(len2 - 1.0f) > 1e-4f) fails++;               /* unit length */
        if (fabsf(tx - 1.0f) > 1e-4f || fabsf(ty) > 1e-4f || fabsf(tz) > 1e-4f) fails++;
        if (fabsf(w) != 1.0f) fails++;                          /* handedness set */
    }
    if (fails) {
        printf("TANCHK: FAIL (%d bad vertices)\n", fails);
        return 1;
    }
    printf("TANCHK: PASS\n");
    return 0;
}
