#include "gltf_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

int g_gltf_debug_enabled = 0;

/* ── Component-type name helper ─────────────────────────────── */
static const char* ct_name(int ct) {
    switch (ct) {
        case 5120: return "BYTE(5120)";
        case 5121: return "UNSIGNED_BYTE(5121)";
        case 5122: return "SHORT(5122)";
        case 5123: return "UNSIGNED_SHORT(5123)";
        case 5125: return "UNSIGNED_INT(5125)";
        case 5126: return "FLOAT(5126)";
        default:   return "UNKNOWN";
    }
}

/* ── Cross product magnitude ────────────────────────────────── */
static float cross_mag(const float v0[3], const float v1[3], const float v2[3]) {
    float e1[3] = {v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2]};
    float e2[3] = {v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2]};
    float cx = e1[1]*e2[2] - e1[2]*e2[1];
    float cy = e1[2]*e2[0] - e1[0]*e2[2];
    float cz = e1[0]*e2[1] - e1[1]*e2[0];
    return sqrtf(cx*cx + cy*cy + cz*cz);
}

/* ── Are two floats bitwise identical? ──────────────────────── */
static int bits_match(float a, float b) {
    unsigned int ia, ib;
    memcpy(&ia, &a, sizeof(ia));
    memcpy(&ib, &b, sizeof(ib));
    return ia == ib;
}

/* ── Accessor metadata helper ───────────────────────────────── */
static void dump_accessor_metadata(
    const GltfAccessor* accs, int na,
    const GltfBufferView* views,
    const GltfBuffer* bufs,
    FILE* fp)
{
    fprintf(fp, "\n=== Section 4: Index Accessor Metadata ===\n");
    for (int i = 0; i < na; i++) {
        const GltfAccessor* a = &accs[i];
        const GltfBufferView* v = &views[a->buffer_view];
        int abs_offset = v->byte_offset + a->byte_offset;
        fprintf(fp,
            "  acc=%d  componentType=%s  count=%d  byteOffset=%d  "
            "bv.byteOffset=%d  bv.byteLength=%d  bv.byteStride=%d  "
            "absByteOffset=%d\n",
            i, ct_name(a->component_type), a->count, a->byte_offset,
            v->byte_offset, v->byte_length, v->byte_stride,
            abs_offset);
    }
}

/* ── Main debug function ────────────────────────────────────── */
void gltf_debug_print(
    const GltfBuffer* bufs, int nb,
    const GltfBufferView* views, int nv,
    const GltfAccessor* accs, int na,
    const GltfMeshRef* mesh_refs, int nm,
    const GltfMeshData* meshes,
    const char* gltf_path)
{
    FILE* fp_out = stdout;
    FILE* fp_json = fopen("mesh_stats.json", "w");
    if (!fp_json) fp_json = stdout;

    fprintf(fp_out, "\n");
    fprintf(fp_out, "========================================\n");
    fprintf(fp_out, "  glTF Mesh Diagnostic Report\n");
    fprintf(fp_out, "  File: %s\n", gltf_path ? gltf_path : "(null)");
    fprintf(fp_out, "========================================\n");
    fprintf(fp_out, "\n");

    /* ── 4. Accessor metadata first (all accessors) ─────────── */
    dump_accessor_metadata(accs, na, views, bufs, fp_out);

    /* ── 1. Degenerate triangle analysis ────────────────────── */
    fprintf(fp_out, "\n=== Section 1: Degenerate Triangle Analysis ===\n");

    for (int mi = 0; mi < nm; mi++) {
        const GltfMeshData* md = &meshes[mi];
        int total_tris = 0;
        int zero = 0, lt_1e9 = 0, lt_1e6 = 0;
        float min_mag = FLT_MAX, max_mag = 0.0f, sum_mag = 0.0f;
        int bitwise_dup = 0;

        for (int pi = 0; pi < md->num_prims; pi++) {
            const GltfPrimitiveData* pd = &md->prims[pi];
            if (!pd->positions || !pd->indices) continue;
            int nt = pd->num_indices / 3;
            for (int t = 0; t < nt; t++) {
                int i0 = pd->indices[t*3];
                int i1 = pd->indices[t*3+1];
                int i2 = pd->indices[t*3+2];
                float v0[3] = {pd->positions[i0*3], pd->positions[i0*3+1], pd->positions[i0*3+2]};
                float v1[3] = {pd->positions[i1*3], pd->positions[i1*3+1], pd->positions[i1*3+2]};
                float v2[3] = {pd->positions[i2*3], pd->positions[i2*3+1], pd->positions[i2*3+2]};
                float mag = cross_mag(v0, v1, v2);
                total_tris++;
                if (mag == 0.0f) zero++;
                if (mag < 1e-9f) lt_1e9++;
                if (mag < 1e-6f) lt_1e6++;
                if (mag < min_mag) min_mag = mag;
                if (mag > max_mag) max_mag = mag;
                sum_mag += mag;
                if (bits_match(v0[0], v1[0]) && bits_match(v0[1], v1[1]) && bits_match(v0[2], v1[2])) bitwise_dup++;
                else if (bits_match(v0[0], v2[0]) && bits_match(v0[1], v2[1]) && bits_match(v0[2], v2[2])) bitwise_dup++;
                else if (bits_match(v1[0], v2[0]) && bits_match(v1[1], v2[1]) && bits_match(v1[2], v2[2])) bitwise_dup++;
            }
        }

        float mean = total_tris > 0 ? sum_mag / total_tris : 0.0f;
        fprintf(fp_out, "\n  Mesh %d (%d prims, %d total tris):\n", mi, md->num_prims, total_tris);
        fprintf(fp_out, "    degenerate (mag==0):        %d\n", zero);
        fprintf(fp_out, "    degenerate (mag<1e-9):      %d\n", lt_1e9);
        fprintf(fp_out, "    degenerate (mag<1e-6):      %d\n", lt_1e6);
        fprintf(fp_out, "    cross-mag min:              %e\n", min_mag);
        fprintf(fp_out, "    cross-mag max:              %e\n", max_mag);
        fprintf(fp_out, "    cross-mag mean:             %e\n", mean);
        fprintf(fp_out, "    bitwise-identical verts:    %d\n", bitwise_dup);
    }

    /* ── 2. First 5 triangles of each mesh ──────────────────── */
    fprintf(fp_out, "\n=== Section 2: First 5 Triangles Per Mesh ===\n");

    for (int mi = 0; mi < nm; mi++) {
        const GltfMeshData* md = &meshes[mi];
        int printed = 0;

        for (int pi = 0; pi < md->num_prims && printed < 5; pi++) {
            const GltfPrimitiveData* pd = &md->prims[pi];
            if (!pd->positions || !pd->indices) continue;
            int nt = pd->num_indices / 3;
            for (int t = 0; t < nt && printed < 5; t++) {
                int i0 = pd->indices[t*3];
                int i1 = pd->indices[t*3+1];
                int i2 = pd->indices[t*3+2];
                float v0[3] = {pd->positions[i0*3], pd->positions[i0*3+1], pd->positions[i0*3+2]};
                float v1[3] = {pd->positions[i1*3], pd->positions[i1*3+1], pd->positions[i1*3+2]};
                float v2[3] = {pd->positions[i2*3], pd->positions[i2*3+1], pd->positions[i2*3+2]};
                fprintf(fp_out, "\n  Mesh %d, tri %d (global):\n", mi, t);
                fprintf(fp_out, "    indices: %d %d %d\n", i0, i1, i2);
                fprintf(fp_out, "    v0: %.10e  %.10e  %.10e\n", v0[0], v0[1], v0[2]);
                fprintf(fp_out, "    v1: %.10e  %.10e  %.10e\n", v1[0], v1[1], v1[2]);
                fprintf(fp_out, "    v2: %.10e  %.10e  %.10e\n", v2[0], v2[1], v2[2]);
                printed++;
            }
        }
    }

    /* ── 3. Index range check ───────────────────────────────── */
    fprintf(fp_out, "\n=== Section 3: Index Range Check ===\n");

    for (int mi = 0; mi < nm; mi++) {
        const GltfMeshData* md = &meshes[mi];
        for (int pi = 0; pi < md->num_prims; pi++) {
            const GltfPrimitiveData* pd = &md->prims[pi];
            if (!pd->indices) continue;
            int min_idx = pd->num_indices > 0 ? pd->indices[0] : 0;
            int max_idx = pd->num_indices > 0 ? pd->indices[0] : 0;
            for (int i = 0; i < pd->num_indices; i++) {
                if (pd->indices[i] < min_idx) min_idx = pd->indices[i];
                if (pd->indices[i] > max_idx) max_idx = pd->indices[i];
            }
            fprintf(fp_out, "  Mesh %d, prim %d: min_idx=%d  max_idx=%d  vert_count=%d",
                    mi, pi, min_idx, max_idx, pd->num_verts);
            if (max_idx >= pd->num_verts) {
                fprintf(fp_out, "  *** FLAG: max_idx >= vert_count ***");
            }
            fprintf(fp_out, "\n");
        }
    }

    /* ── JSON output ────────────────────────────────────────── */
    if (fp_json && fp_json != stdout) {
        fprintf(fp_json, "{\n");
        fprintf(fp_json, "  \"file\": \"%s\",\n", gltf_path ? gltf_path : "null");
        fprintf(fp_json, "  \"meshes\": [\n");

        for (int mi = 0; mi < nm; mi++) {
            const GltfMeshData* md = &meshes[mi];
            fprintf(fp_json, "    {\n");
            fprintf(fp_json, "      \"mesh_index\": %d,\n", mi);
            fprintf(fp_json, "      \"num_prims\": %d,\n", md->num_prims);
            fprintf(fp_json, "      \"prims\": [\n");

            for (int pi = 0; pi < md->num_prims; pi++) {
                const GltfPrimitiveData* pd = &md->prims[pi];
                int nt = (pd->positions && pd->indices) ? pd->num_indices / 3 : 0;
                int degenerate_0 = 0, degenerate_1e9 = 0, degenerate_1e6 = 0;
                float min_m = FLT_MAX, max_m = 0.0f, sum_m = 0.0f;
                int bitwise = 0;
                int min_idx = pd->num_indices > 0 ? pd->indices[0] : 0;
                int max_idx = pd->num_indices > 0 ? pd->indices[0] : 0;

                for (int i = 0; i < pd->num_indices; i++) {
                    if (pd->indices[i] < min_idx) min_idx = pd->indices[i];
                    if (pd->indices[i] > max_idx) max_idx = pd->indices[i];
                }

                for (int t = 0; t < nt; t++) {
                    int i0 = pd->indices[t*3];
                    int i1 = pd->indices[t*3+1];
                    int i2 = pd->indices[t*3+2];
                    float v0[3] = {pd->positions[i0*3], pd->positions[i0*3+1], pd->positions[i0*3+2]};
                    float v1[3] = {pd->positions[i1*3], pd->positions[i1*3+1], pd->positions[i1*3+2]};
                    float v2[3] = {pd->positions[i2*3], pd->positions[i2*3+1], pd->positions[i2*3+2]};
                    float mag = cross_mag(v0, v1, v2);
                    if (mag == 0.0f) degenerate_0++;
                    if (mag < 1e-9f) degenerate_1e9++;
                    if (mag < 1e-6f) degenerate_1e6++;
                    if (mag < min_m) min_m = mag;
                    if (mag > max_m) max_m = mag;
                    sum_m += mag;
                    if (bits_match(v0[0], v1[0]) && bits_match(v0[1], v1[1]) && bits_match(v0[2], v1[2])) bitwise++;
                    else if (bits_match(v0[0], v2[0]) && bits_match(v0[1], v2[1]) && bits_match(v0[2], v2[2])) bitwise++;
                    else if (bits_match(v1[0], v2[0]) && bits_match(v1[1], v2[1]) && bits_match(v1[2], v2[2])) bitwise++;
                }

                float mean = nt > 0 ? sum_m / nt : 0.0f;
                fprintf(fp_json, "        {\n");
                fprintf(fp_json, "          \"prim_index\": %d,\n", pi);
                fprintf(fp_json, "          \"num_verts\": %d,\n", pd->num_verts);
                fprintf(fp_json, "          \"num_indices\": %d,\n", pd->num_indices);
                fprintf(fp_json, "          \"num_tris\": %d,\n", nt);
                fprintf(fp_json, "          \"material\": %d,\n", pd->material);
                fprintf(fp_json, "          \"degenerate_mag_0\": %d,\n", degenerate_0);
                fprintf(fp_json, "          \"degenerate_mag_1e-9\": %d,\n", degenerate_1e9);
                fprintf(fp_json, "          \"degenerate_mag_1e-6\": %d,\n", degenerate_1e6);
                fprintf(fp_json, "          \"cross_mag_min\": %e,\n", min_m);
                fprintf(fp_json, "          \"cross_mag_max\": %e,\n", max_m);
                fprintf(fp_json, "          \"cross_mag_mean\": %e,\n", mean);
                fprintf(fp_json, "          \"bitwise_identical\": %d,\n", bitwise);
                fprintf(fp_json, "          \"min_idx\": %d,\n", min_idx);
                fprintf(fp_json, "          \"max_idx\": %d,\n", max_idx);
                fprintf(fp_json, "          \"index_oob\": %s\n",
                        (max_idx >= pd->num_verts) ? "true" : "false");
                if (pi < md->num_prims - 1)
                    fprintf(fp_json, "        },\n");
                else
                    fprintf(fp_json, "        }\n");
            }

            fprintf(fp_json, "      ]\n");
            if (mi < nm - 1)
                fprintf(fp_json, "    },\n");
            else
                fprintf(fp_json, "    }\n");
        }

        fprintf(fp_json, "  ]\n");
        fprintf(fp_json, "}\n");
        fclose(fp_json);
    }

    fprintf(fp_out, "\n========================================\n");
    fprintf(fp_out, "  End of Diagnostic Report\n");
    fprintf(fp_out, "========================================\n");
    fflush(fp_out);
}
/* ── Renderer-level global-array diagnostics ────────────────── */
void gltf_debug_global_arrays(
    const Scene* scene,
    const TriGpu* all_tris, int total_tris,
    const int* tri_offset,
    const BvhNode* all_bvh, int num_bvh_nodes,
    int pass)
{
    FILE* fp = stdout;
    const char* label = (pass == 0) ? "BEFORE BVH" : "AFTER BVH";

    fprintf(fp, "\n");
    fprintf(fp, "========================================\n");
    fprintf(fp, "  Global Array Diagnostic (%s)\n", label);
    fprintf(fp, "  total_tris=%d  num_meshes=%d\n", total_tris, scene->num_meshes);
    fprintf(fp, "========================================\n");

    /* ── (1) Per-mesh metadata + offsets ───────────────────── */
    fprintf(fp, "\n--- (1) Per-Mesh Offsets into Global Arrays ---\n");
    for (int m = 0; m < scene->num_meshes; m++) {
        const MeshObj* mo = &scene->meshes[m];
        fprintf(fp, "  mesh[%d]  name=\"%s\"  tris=%d  verts=%d  "
                    "tri_offset=%d  vert_offset=%d\n",
                m, mo->material, mo->num_tris, mo->num_tris * 3,
                tri_offset ? tri_offset[m] : -1,
                tri_offset ? tri_offset[m] * 3 : -1);
    }

    /* ── (2) First and last triangle from each mesh in global array ── */
    fprintf(fp, "\n--- (2) First & Last Triangle in Global Array Per Mesh ---\n");
    for (int m = 0; m < scene->num_meshes; m++) {
        const MeshObj* mo = &scene->meshes[m];
        int off = tri_offset ? tri_offset[m] : 0;
        int nt = mo->num_tris;

        if (nt <= 0) {
            fprintf(fp, "  mesh[%d]: no triangles\n", m);
            continue;
        }

        /* Compute this mesh's own bbox from its local tris (before BVH) */
        float local_min[3] = {1e9f,1e9f,1e9f};
        float local_max[3] = {-1e9f,-1e9f,-1e9f};
        for (int t = 0; t < nt; t++) {
            const float* v[3] = {mo->tris[t].v0, mo->tris[t].v1, mo->tris[t].v2};
            for (int k = 0; k < 3; k++) {
                if (v[0][k] < local_min[k]) local_min[k] = v[0][k];
                if (v[1][k] < local_min[k]) local_min[k] = v[1][k];
                if (v[2][k] < local_min[k]) local_min[k] = v[2][k];
                if (v[0][k] > local_max[k]) local_max[k] = v[0][k];
                if (v[1][k] > local_max[k]) local_max[k] = v[1][k];
                if (v[2][k] > local_max[k]) local_max[k] = v[2][k];
            }
        }

        /* Tri 0 in global array */
        const TriGpu* t0 = &all_tris[off];
        fprintf(fp, "\n  mesh[%d]  name=\"%s\"  local_bbox=[%.6e %.6e %.6e] x [%.6e %.6e %.6e]\n",
                m, mo->material,
                local_min[0], local_min[1], local_min[2],
                local_max[0], local_max[1], local_max[2]);

        float t0_min[3], t0_max[3];
        for (int k = 0; k < 3; k++) {
            t0_min[k] = fminf(fminf(t0->v0[k], t0->v1[k]), t0->v2[k]);
            t0_max[k] = fmaxf(fmaxf(t0->v0[k], t0->v1[k]), t0->v2[k]);
        }
        fprintf(fp, "    global_tri[off+0]:  v0=(%.10e,%.10e,%.10e)  v1=(%.10e,%.10e,%.10e)  v2=(%.10e,%.10e,%.10e)\n",
                t0->v0[0], t0->v0[1], t0->v0[2],
                t0->v1[0], t0->v1[1], t0->v1[2],
                t0->v2[0], t0->v2[1], t0->v2[2]);
        fprintf(fp, "      tri_bbox=[%.6e %.6e %.6e] x [%.6e %.6e %.6e]\n",
                t0_min[0], t0_min[1], t0_min[2], t0_max[0], t0_max[1], t0_max[2]);

        /* Last tri in global array */
        const TriGpu* tL = &all_tris[off + nt - 1];
        float tL_min[3], tL_max[3];
        for (int k = 0; k < 3; k++) {
            tL_min[k] = fminf(fminf(tL->v0[k], tL->v1[k]), tL->v2[k]);
            tL_max[k] = fmaxf(fmaxf(tL->v0[k], tL->v1[k]), tL->v2[k]);
        }
        fprintf(fp, "    global_tri[off+%d]: v0=(%.10e,%.10e,%.10e)  v1=(%.10e,%.10e,%.10e)  v2=(%.10e,%.10e,%.10e)\n",
                nt - 1,
                tL->v0[0], tL->v0[1], tL->v0[2],
                tL->v1[0], tL->v1[1], tL->v1[2],
                tL->v2[0], tL->v2[1], tL->v2[2]);
        fprintf(fp, "      tri_bbox=[%.6e %.6e %.6e] x [%.6e %.6e %.6e]\n",
                tL_min[0], tL_min[1], tL_min[2], tL_max[0], tL_max[1], tL_max[2]);

        /* Confirm vertices match mesh bbox */
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            if (t0_min[k] < local_min[k] - 1e-6f || t0_max[k] > local_max[k] + 1e-6f) ok = 0;
            if (tL_min[k] < local_min[k] - 1e-6f || tL_max[k] > local_max[k] + 1e-6f) ok = 0;
        }
        fprintf(fp, "    bbox_ok=%s\n", ok ? "YES" : "NO");
    }

    /* ── (3) Note on Section 2 indices ──────────────────────── */
    fprintf(fp, "\n--- (3) Section 2 Index Interpretation ---\n");
    fprintf(fp, "  Section 2 uses indices from decoded glTF primitive data\n");
    fprintf(fp, "  (pd->indices), which are LOCAL to each mesh's own vertex\n");
    fprintf(fp, "  array — they are pre-offset, pre-BVH indices into each\n");
    fprintf(fp, "  mesh's private positions[] array (range [0, num_verts-1]).\n");
    fprintf(fp, "  The GPU global array stores triangles as baked float[3]\n");
    fprintf(fp, "  vertices, not indexed — there is no global vertex array.\n");

    fprintf(fp, "\n========================================\n");
    fprintf(fp, "  End of Global Array Diagnostic (%s)\n", label);
    fprintf(fp, "========================================\n");
    fflush(fp);
}
