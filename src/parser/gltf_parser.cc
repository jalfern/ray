#include "gltf_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ── JSON token helpers ──────────────────────────────────────────
 *
 * glTF is plain JSON with no extensions.  These helpers navigate a
 * JSON byte buffer by pointer arithmetic — no tokeniser, no allocs
 * beyond the input buffer.  Every function advances *p past the
 * consumed token and returns 1 on success, 0 on failure.
 */

static int skip_ws_ptr(const char** p) {
    while (**p && (unsigned char)**p <= 0x20) (*p)++;
    return 1;
}

/* Peek the next non-whitespace character without advancing. */
static char peek(const char** p) {
    const char* tmp = *p;
    skip_ws_ptr(&tmp);
    return *tmp;
}

/* Expect and consume a single character c.  Skip whitespace first. */
static int expect_char(const char** p, char c) {
    skip_ws_ptr(p);
    if (**p != c) return 0;
    (*p)++;
    return 1;
}

/* Parse a JSON string into buf (null-terminated, max_len including
 * the terminator).  Advances *p past the closing quote. */
static int parse_json_string(const char** p, char* buf, int max_len) {
    skip_ws_ptr(p);
    if (**p != '"') return 0;
    (*p)++;
    int i = 0;
    while (**p && **p != '"' && i < max_len - 1) {
        if (**p == '\\' && *(*p + 1)) {
            (*p)++;
            switch (**p) {
                case 'n': buf[i++] = '\n'; break;
                case 't': buf[i++] = '\t'; break;
                case '\\': buf[i++] = '\\'; break;
                case '"':  buf[i++] = '"';  break;
                default:   buf[i++] = **p;  break;
            }
        } else {
            buf[i++] = **p;
        }
        (*p)++;
    }
    buf[i] = '\0';
    if (**p == '"') (*p)++;
    return 1;
}

/* Parse a JSON number (integer or float) into val. */
static int parse_json_number(const char** p, float* val) {
    skip_ws_ptr(p);
    char* end;
    *val = strtof(*p, &end);
    if (end == *p) return 0;
    *p = end;
    return 1;
}

static int parse_json_int(const char** p, int* val) {
    skip_ws_ptr(p);
    char* end;
    *val = (int)strtol(*p, &end, 10);
    if (end == *p) return 0;
    *p = end;
    return 1;
}

/* Skip over any JSON value (string, number, object, array, true,
 * false, null) without interpreting it. */
static void skip_value(const char** p) {
    skip_ws_ptr(p);
    if (**p == '"') {
        (*p)++;
        while (**p && (**p != '"' || *(*p - 1) == '\\')) (*p)++;
        if (**p == '"') (*p)++;
    } else if (**p == '[') {
        (*p)++;
        skip_ws_ptr(p);
        while (**p && **p != ']') {
            skip_value(p);
            skip_ws_ptr(p);
            if (**p == ',') (*p)++;
        }
        if (**p == ']') (*p)++;
    } else if (**p == '{') {
        (*p)++;
        skip_ws_ptr(p);
        while (**p && **p != '}') {
            skip_value(p);  /* key */
            skip_ws_ptr(p);
            if (**p == ':') (*p)++;
            skip_value(p);  /* value */
            skip_ws_ptr(p);
            if (**p == ',') (*p)++;
        }
        if (**p == '}') (*p)++;
    } else {
        while (**p && !isspace(**p) && **p != ',' && **p != ']' && **p != '}') (*p)++;
    }
}

/* ── Schema-aware navigation ─────────────────────────────────────
 *
 * These functions expect glTF's known object layout.  They do NOT
 * do a general JSON parse — they find specific keys at the current
 * object scope.
 */

/* Find a key within the current JSON object.  Returns a pointer to
 * the first character of the key's value, or NULL if not found.
 * Does NOT advance *p past the object. */
static const char* find_key(const char** p, const char* key) {
    const char* cur = *p;
    skip_ws_ptr(&cur);
    if (*cur != '{') return NULL;
    cur++;
    while (*cur && *cur != '}') {
        char kbuf[128];
        const char* save = cur;
        if (!parse_json_string(&cur, kbuf, sizeof(kbuf))) { cur = save; skip_value(&cur); continue; }
        skip_ws_ptr(&cur);
        if (*cur == ':') cur++;
        skip_ws_ptr(&cur);
        if (strcmp(kbuf, key) == 0) return cur;
        skip_value(&cur);
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    return NULL;
}

/* Find key and return a pointer positioned at its value, advancing
 * *p to just after the value.  Returns 1 if found, 0 if not. */
static int seek_key(const char** p, const char* key) {
    const char* cur = *p;
    skip_ws_ptr(&cur);
    if (*cur != '{') return 0;
    cur++;
    while (*cur && *cur != '}') {
        char kbuf[128];
        const char* save = cur;
        if (!parse_json_string(&cur, kbuf, sizeof(kbuf))) { cur = save; skip_value(&cur); continue; }
        skip_ws_ptr(&cur);
        if (*cur == ':') cur++;
        skip_ws_ptr(&cur);
        if (strcmp(kbuf, key) == 0) { *p = cur; return 1; }
        skip_value(&cur);
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    return 0;
}

/* Parse a JSON array of floats into out.  Returns number read. */
static int parse_float_array(const char** p, float* out, int max) {
    skip_ws_ptr(p);
    int count = 0;
    if (**p != '[') return 0;
    (*p)++;
    while (**p && **p != ']' && count < max) {
        skip_ws_ptr(p);
        if (**p == ']') break;
        if (!parse_json_number(p, &out[count])) break;
        count++;
        skip_ws_ptr(p);
        if (**p == ',') (*p)++;
    }
    if (**p == ']') (*p)++;
    return count;
}

/* Parse a JSON array of integers into out.  Returns number read. */
static int parse_int_array(const char** p, int* out, int max) {
    skip_ws_ptr(p);
    int count = 0;
    if (**p != '[') return 0;
    (*p)++;
    while (**p && **p != ']' && count < max) {
        skip_ws_ptr(p);
        if (**p == ']') break;
        if (!parse_json_int(p, &out[count])) break;
        count++;
        skip_ws_ptr(p);
        if (**p == ',') (*p)++;
    }
    if (**p == ']') (*p)++;
    return count;
}

/* ── Internal glTF data model ────────────────────────────────── */

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
    int byte_stride;   /* 0 means tightly packed */
} GltfBufferView;

typedef struct {
    int buffer_view;
    int byte_offset;
    int component_type;
    int count;
    int num_components; /* 1=SCALAR,2=VEC2,3=VEC3,4=VEC4 */
} GltfAccessor;

/* ── Component-type helpers ──────────────────────────────────── */

static int component_byte_size(int ct) {
    switch (ct) {
        case 5120: return 1; /* BYTE */
        case 5121: return 1; /* UNSIGNED_BYTE */
        case 5122: return 2; /* SHORT */
        case 5123: return 2; /* UNSIGNED_SHORT */
        case 5125: return 4; /* UNSIGNED_INT */
        case 5126: return 4; /* FLOAT */
        default:   return 0;
    }
}

static int type_to_components(const char* type) {
    if (strcmp(type, "SCALAR") == 0) return 1;
    if (strcmp(type, "VEC2") == 0)   return 2;
    if (strcmp(type, "VEC3") == 0)   return 3;
    if (strcmp(type, "VEC4") == 0)   return 4;
    if (strcmp(type, "MAT2") == 0)   return 4;
    if (strcmp(type, "MAT3") == 0)   return 9;
    if (strcmp(type, "MAT4") == 0)   return 16;
    return 0;
}

/* ── Decode a single accessor into a flat float array ──────────
 *
 *  Returns a malloc'd float[count * num_components] or NULL on
 *  error.  For index accessors (USHORT / UINT) the caller casts
 *  the bits to int after receiving them as floats — or we offer
 *  a separate decode path.  We provide both: decode_acc_f32 for
 *  vertex data and decode_acc_idx for triangle indices.
 */

static float* decode_acc_f32(
    const GltfAccessor* acc,
    const GltfBufferView* views,
    const GltfBuffer* bufs)
{
    const GltfBufferView* vw = &views[acc->buffer_view];
    const GltfBuffer* buf = &bufs[vw->buffer];

    int comp_bytes = component_byte_size(acc->component_type);
    int elem_bytes = comp_bytes * acc->num_components;
    int stride = vw->byte_stride ? vw->byte_stride : elem_bytes;

    float* out = (float*)malloc(acc->count * acc->num_components * sizeof(float));
    if (!out) return NULL;

    int src_offset = vw->byte_offset + acc->byte_offset;

    if (acc->component_type == 5126) {
        /* FLOAT — direct memcpy */
        for (int i = 0; i < acc->count; i++) {
            const unsigned char* src = buf->data + src_offset + i * stride;
            memcpy(out + i * acc->num_components, src, elem_bytes);
        }
    } else {
        /* Integer types — cast to float */
        for (int i = 0; i < acc->count; i++) {
            const unsigned char* src = buf->data + src_offset + i * stride;
            for (int c = 0; c < acc->num_components; c++) {
                float v = 0.0f;
                switch (acc->component_type) {
                    case 5120: v = (float)*(signed char*)(src + c * comp_bytes); break;
                    case 5121: v = (float)*(unsigned char*)(src + c * comp_bytes); break;
                    case 5122: v = (float)*(signed short*)(src + c * comp_bytes); break;
                    case 5123: v = (float)*(unsigned short*)(src + c * comp_bytes); break;
                    case 5125: v = (float)*(unsigned int*)(src + c * comp_bytes); break;
                }
                out[i * acc->num_components + c] = v;
            }
        }
    }
    return out;
}

static int* decode_acc_idx(
    const GltfAccessor* acc,
    const GltfBufferView* views,
    const GltfBuffer* bufs)
{
    const GltfBufferView* vw = &views[acc->buffer_view];
    const GltfBuffer* buf = &bufs[vw->buffer];
    int comp_bytes = component_byte_size(acc->component_type);
    int stride = vw->byte_stride ? vw->byte_stride : comp_bytes;
    int src_offset = vw->byte_offset + acc->byte_offset;

    int* out = (int*)malloc(acc->count * sizeof(int));
    if (!out) return NULL;

    for (int i = 0; i < acc->count; i++) {
        const unsigned char* src = buf->data + src_offset + i * stride;
        switch (acc->component_type) {
            case 5123: out[i] = *(unsigned short*)src; break;
            case 5125: out[i] = *(unsigned int*)src;   break;
            default:   out[i] = 0; break;
        }
    }
    return out;
}

/* ── Parse the top-level glTF arrays ─────────────────────────── */

static int parse_buffers(const char** j, GltfBuffer* bufs, int max, const char* base_dir) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&bufs[n], 0, sizeof(bufs[n]));

    int byte_len = 0;
        char uri[512]; uri[0] = 0;

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            if (strcmp(kbuf, "byteLength") == 0) {
                float f; if (parse_json_number(&obj, &f)) byte_len = (int)f;
            } else if (strcmp(kbuf, "uri") == 0) {
                parse_json_string(&obj, uri, sizeof(uri));
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;

        if (uri[0]) {
            char full[1024];
            if (uri[0] == '/') {
                snprintf(full, sizeof(full), "%s", uri);
            } else {
                snprintf(full, sizeof(full), "%s/%s", base_dir, uri);
            }
            FILE* f = fopen(full, "rb");
            if (f) {
                bufs[n].byte_length = byte_len;
                bufs[n].data = (unsigned char*)malloc(byte_len);
                if (bufs[n].data) {
                    size_t got = fread(bufs[n].data, 1, byte_len, f);
                    (void)got;
                }
                fclose(f);
            }
        }
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

static int parse_buffer_views(const char** j, GltfBufferView* views, int max) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&views[n], 0, sizeof(views[n]));
        views[n].byte_stride = 0;

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            float fv;
            if (strcmp(kbuf, "buffer") == 0) {
                if (parse_json_number(&obj, &fv)) views[n].buffer = (int)fv;
            } else if (strcmp(kbuf, "byteOffset") == 0) {
                if (parse_json_number(&obj, &fv)) views[n].byte_offset = (int)fv;
            } else if (strcmp(kbuf, "byteLength") == 0) {
                if (parse_json_number(&obj, &fv)) views[n].byte_length = (int)fv;
            } else if (strcmp(kbuf, "byteStride") == 0) {
                if (parse_json_number(&obj, &fv)) views[n].byte_stride = (int)fv;
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

static int parse_accessors(const char** j, GltfAccessor* accs, int max) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&accs[n], 0, sizeof(accs[n]));

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            float fv;
            if (strcmp(kbuf, "bufferView") == 0) {
                if (parse_json_number(&obj, &fv)) accs[n].buffer_view = (int)fv;
            } else if (strcmp(kbuf, "byteOffset") == 0) {
                if (parse_json_number(&obj, &fv)) accs[n].byte_offset = (int)fv;
            } else if (strcmp(kbuf, "componentType") == 0) {
                if (parse_json_number(&obj, &fv)) accs[n].component_type = (int)fv;
            } else if (strcmp(kbuf, "count") == 0) {
                if (parse_json_number(&obj, &fv)) accs[n].count = (int)fv;
            } else if (strcmp(kbuf, "type") == 0) {
                char type_str[16];
                if (parse_json_string(&obj, type_str, sizeof(type_str)))
                    accs[n].num_components = type_to_components(type_str);
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

/* ── Raw decoded primitive data (pre-transform) ─────────────── */

typedef struct {
    float* positions;   /* [num_verts * 3] */
    float* normals;     /* [num_verts * 3] */
    int*   indices;     /* [num_indices] */
    int num_verts;
    int num_indices;
    int material;       /* index into materials[] */
} GltfPrimitiveData;

typedef struct {
    GltfPrimitiveData prims[MAX_PRIMITIVES];
    int num_prims;
} GltfMeshData;

/* ── Intermediate JSON-only mesh refs (no decode yet) ────────── */

typedef struct {
    int pos_acc;
    int norm_acc;
    int idx_acc;
    int material;
} GltfPrimitiveRef;

typedef struct {
    GltfPrimitiveRef prims[MAX_PRIMITIVES];
    int num_prims;
} GltfMeshRef;

/* ── Parse a single primitive's attributes & indices ───────────
 *
 *  Given a pointer into a primitive JSON object, decode the
 *  accessors referenced by POSITION, NORMAL, and indices, and
 *  fill *out.  Returns 1 on success.
 */

static int parse_primitive(const char** j,
                           GltfPrimitiveData* out,
                           const GltfAccessor* accs, int na,
                           const GltfBufferView* views,
                           const GltfBuffer* bufs)
{
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '{') return 0;
    cur++;

    int pos_acc = -1, norm_acc = -1, idx_acc = -1;
    int material = -1;

    while (*cur && *cur != '}') {
        char kbuf[128];
        const char* save = cur;
        if (!parse_json_string(&cur, kbuf, sizeof(kbuf))) { cur = save; skip_value(&cur); continue; }
        skip_ws_ptr(&cur);
        if (*cur == ':') cur++;
        skip_ws_ptr(&cur);
        float fv;
        if (strcmp(kbuf, "attributes") == 0) {
            /* attributes is an object: {"POSITION": i, "NORMAL": i, ...} */
            const char* attr = cur;
            skip_ws_ptr(&attr);
            if (*attr == '{') attr++;
            while (*attr && *attr != '}') {
                char ak[64];
                const char* asave = attr;
                if (!parse_json_string(&attr, ak, sizeof(ak))) { attr = asave; skip_value(&attr); continue; }
                skip_ws_ptr(&attr);
                if (*attr == ':') attr++;
                skip_ws_ptr(&attr);
                if (parse_json_number(&attr, &fv)) {
                    int av = (int)fv;
                    if (strcmp(ak, "POSITION") == 0) pos_acc = av;
                    else if (strcmp(ak, "NORMAL") == 0) norm_acc = av;
                }
                skip_ws_ptr(&attr);
                if (*attr == ',') attr++;
            }
            if (*attr == '}') attr++;
            cur = attr;
        } else if (strcmp(kbuf, "indices") == 0) {
            if (parse_json_number(&cur, &fv)) idx_acc = (int)fv;
        } else if (strcmp(kbuf, "material") == 0) {
            if (parse_json_number(&cur, &fv)) material = (int)fv;
        } else {
            skip_value(&cur);
        }
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == '}') cur++;
    *j = cur;

    memset(out, 0, sizeof(*out));
    out->material = material;

    /* Decode positions */
    if (pos_acc >= 0 && pos_acc < na) {
        float* p = decode_acc_f32(&accs[pos_acc], views, bufs);
        if (p) {
            out->positions = p;
            out->num_verts = accs[pos_acc].count;
        }
    }
    /* Decode normals */
    if (norm_acc >= 0 && norm_acc < na) {
        float* n = decode_acc_f32(&accs[norm_acc], views, bufs);
        if (n) out->normals = n;
    }
    /* Decode indices */
    if (idx_acc >= 0 && idx_acc < na) {
        int* ix = decode_acc_idx(&accs[idx_acc], views, bufs);
        if (ix) {
            out->indices = ix;
            out->num_indices = accs[idx_acc].count;
        }
    }
    return 1;
}

/* ── Parse the meshes array (JSON only, no decode) ──────────── */

static int parse_mesh_refs(const char** j,
                           GltfMeshRef* refs, int max)
{
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    int guard = 0;
    while (*cur && *cur != ']' && n < max && guard < 10000) {
        guard++;
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }

        memset(&refs[n], 0, sizeof(refs[n]));

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        int inner_guard = 0;
        while (*obj && *obj != '}' && inner_guard < 1000) {
            inner_guard++;
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            if (strcmp(kbuf, "primitives") == 0) {
                const char* arr = obj;
                skip_ws_ptr(&arr);
                if (*arr == '[') arr++;
                int prim_guard = 0;
                while (*arr && *arr != ']' && refs[n].num_prims < MAX_PRIMITIVES && prim_guard < 10000) {
                    prim_guard++;
                    GltfPrimitiveRef pr;
                    memset(&pr, 0, sizeof(pr));
                    pr.pos_acc = -1;
                    pr.norm_acc = -1;
                    pr.idx_acc = -1;
                    pr.material = -1;

                    const char* pcur = arr;
                    skip_ws_ptr(&pcur);
                    if (*pcur == '{') pcur++;
                    while (*pcur && *pcur != '}') {
                        char pk[64];
                        const char* psave = pcur;
                        if (!parse_json_string(&pcur, pk, sizeof(pk))) { pcur = psave; skip_value(&pcur); continue; }
                        skip_ws_ptr(&pcur);
                        if (*pcur == ':') pcur++;
                        skip_ws_ptr(&pcur);
                        float fv;
                        if (strcmp(pk, "attributes") == 0) {
                            const char* attr = pcur;
                            skip_ws_ptr(&attr);
                            if (*attr == '{') attr++;
                            while (*attr && *attr != '}') {
                                char ak[64];
                                const char* asave = attr;
                                if (!parse_json_string(&attr, ak, sizeof(ak))) { attr = asave; skip_value(&attr); continue; }
                                skip_ws_ptr(&attr);
                                if (*attr == ':') attr++;
                                skip_ws_ptr(&attr);
                                if (parse_json_number(&attr, &fv)) {
                                    int av = (int)fv;
                                    if (strcmp(ak, "POSITION") == 0) pr.pos_acc = av;
                                    else if (strcmp(ak, "NORMAL") == 0) pr.norm_acc = av;
                                }
                                skip_ws_ptr(&attr);
                                if (*attr == ',') attr++;
                            }
                            if (*attr == '}') attr++;
                            pcur = attr;
                        } else if (strcmp(pk, "indices") == 0) {
                            if (parse_json_number(&pcur, &fv)) pr.idx_acc = (int)fv;
                        } else if (strcmp(pk, "material") == 0) {
                            if (parse_json_number(&pcur, &fv)) pr.material = (int)fv;
                        } else {
                            skip_value(&pcur);
                        }
                        skip_ws_ptr(&pcur);
                        if (*pcur == ',') pcur++;
                    }
                    if (*pcur == '}') pcur++;
                    arr = pcur;

                    refs[n].prims[refs[n].num_prims++] = pr;
                    skip_ws_ptr(&arr);
                    if (*arr == ',') arr++;
                }
                if (*arr == ']') arr++;
                obj = arr;
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

/* ── Decode pass: turn mesh refs into actual triangle data ──── */

static void decode_meshes(GltfMeshRef* refs, int num_refs,
                          GltfMeshData* out,
                          const GltfAccessor* accs, int na,
                          const GltfBufferView* views,
                          const GltfBuffer* bufs)
{
    for (int mi = 0; mi < num_refs; mi++) {
        memset(&out[mi], 0, sizeof(out[mi]));
        for (int pi = 0; pi < refs[mi].num_prims; pi++) {
            GltfPrimitiveRef* pr = &refs[mi].prims[pi];
            GltfPrimitiveData pd;
            memset(&pd, 0, sizeof(pd));
            pd.material = pr->material;

            if (pr->pos_acc >= 0 && pr->pos_acc < na) {
                float* p = decode_acc_f32(&accs[pr->pos_acc], views, bufs);
                if (p) {
                    pd.positions = p;
                    pd.num_verts = accs[pr->pos_acc].count;
                }
            }
            if (pr->norm_acc >= 0 && pr->norm_acc < na) {
                float* n = decode_acc_f32(&accs[pr->norm_acc], views, bufs);
                if (n) pd.normals = n;
            }
            if (pr->idx_acc >= 0 && pr->idx_acc < na) {
                int* ix = decode_acc_idx(&accs[pr->idx_acc], views, bufs);
                if (ix) {
                    pd.indices = ix;
                    pd.num_indices = accs[pr->idx_acc].count;
                }
            }

            out[mi].prims[out[mi].num_prims++] = pd;
        }
    }
}

/* ── Node / scene / camera data model ───────────────────────── */

typedef struct {
    int children[64];
    int num_children;
    int mesh;          /* -1 = no mesh */
    int camera;        /* -1 = no camera */
    float matrix[16];  /* column-major 4x4, identity if no explicit matrix */
    int has_matrix;    /* 0 = use TRS, 1 = use matrix directly */
    float translation[3];
    float rotation[4]; /* quaternion xyzw */
    float scale[3];
} GltfNode;

typedef struct {
    float yfov;
    float znear, zfar;
} GltfCamera;

typedef struct {
    float base_color[4];   /* RGBA */
    float metallic;
    float roughness;
    float emissive[3];
    float transmission;    /* 0-1, default 0 */
    float ior;             /* 1.0-3.0, default 1.5 */
} GltfMaterial;

/* ── 4×4 matrix helpers (column-major) ──────────────────────── */

static void m4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(float r[16], const float a[16], const float b[16]) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = sum;
        }
    }
}

static void m4_transform(float out[3], const float m[16], const float in[3]) {
    float x = in[0], y = in[1], z = in[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* Transform normal by 3×3 inverse-transpose (upper-left of m). */
static void m4_transform_normal(float out[3], const float m[16], const float in[3]) {
    float x = in[0], y = in[1], z = in[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
    /* renormalise */
    float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    if (len > 1e-8f) { out[0] /= len; out[1] /= len; out[2] /= len; }
}

/* Build a column-major 4×4 from TRS.  The quaternion is xyzw. */
static void m4_from_trs(float m[16], const float t[3], const float q[4], const float s[3]) {
    float xx = q[0]*q[0], yy = q[1]*q[1], zz = q[2]*q[2];
    float xy = q[0]*q[1], xz = q[0]*q[2], xw = q[0]*q[3];
    float yz = q[1]*q[2], yw = q[1]*q[3], zw = q[2]*q[3];
    m[0]  = s[0] * (1.0f - 2.0f*(yy + zz));
    m[1]  = s[0] * (2.0f*(xy + zw));
    m[2]  = s[0] * (2.0f*(xz - yw));
    m[3]  = 0.0f;
    m[4]  = s[1] * (2.0f*(xy - zw));
    m[5]  = s[1] * (1.0f - 2.0f*(xx + zz));
    m[6]  = s[1] * (2.0f*(yz + xw));
    m[7]  = 0.0f;
    m[8]  = s[2] * (2.0f*(xz + yw));
    m[9]  = s[2] * (2.0f*(yz - xw));
    m[10] = s[2] * (1.0f - 2.0f*(xx + yy));
    m[11] = 0.0f;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1.0f;
}

/* ── Parse the nodes array ───────────────────────────────────── */

static int parse_nodes(const char** j, GltfNode* nodes, int max) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&nodes[n], 0, sizeof(nodes[n]));
        nodes[n].mesh = -1;
        nodes[n].camera = -1;
        m4_identity(nodes[n].matrix);
        nodes[n].scale[0] = nodes[n].scale[1] = nodes[n].scale[2] = 1.0f;
        nodes[n].rotation[3] = 1.0f;

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            float fv;
            if (strcmp(kbuf, "mesh") == 0) {
                if (parse_json_number(&obj, &fv)) nodes[n].mesh = (int)fv;
            } else if (strcmp(kbuf, "camera") == 0) {
                if (parse_json_number(&obj, &fv)) nodes[n].camera = (int)fv;
            } else if (strcmp(kbuf, "children") == 0) {
                const char* arr = obj;
                skip_ws_ptr(&arr);
                if (*arr == '[') arr++;
                while (*arr && *arr != ']' && nodes[n].num_children < 64) {
                    skip_ws_ptr(&arr);
                    if (*arr == ']') break;
                    if (parse_json_number(&arr, &fv))
                        nodes[n].children[nodes[n].num_children++] = (int)fv;
                    skip_ws_ptr(&arr);
                    if (*arr == ',') arr++;
                }
                if (*arr == ']') arr++;
                obj = arr;
            } else if (strcmp(kbuf, "matrix") == 0) {
                float m16[16];
                int cnt = parse_float_array(&obj, m16, 16);
                if (cnt == 16) {
                    nodes[n].has_matrix = 1;
                    for (int i = 0; i < 16; i++) nodes[n].matrix[i] = m16[i];
                }
            } else if (strcmp(kbuf, "translation") == 0) {
                parse_float_array(&obj, nodes[n].translation, 3);
            } else if (strcmp(kbuf, "rotation") == 0) {
                parse_float_array(&obj, nodes[n].rotation, 4);
            } else if (strcmp(kbuf, "scale") == 0) {
                parse_float_array(&obj, nodes[n].scale, 3);
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

/* ── Parse the cameras array ─────────────────────────────────── */

static int parse_cameras(const char** j, GltfCamera* cams, int max) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&cams[n], 0, sizeof(cams[n]));

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            if (strcmp(kbuf, "perspective") == 0) {
                const char* p = obj;
                skip_ws_ptr(&p);
                if (*p == '{') p++;
                while (*p && *p != '}') {
                    char pk[64];
                    const char* psave = p;
                    if (!parse_json_string(&p, pk, sizeof(pk))) { p = psave; skip_value(&p); continue; }
                    skip_ws_ptr(&p);
                    if (*p == ':') p++;
                    skip_ws_ptr(&p);
                    float fv;
                    if (strcmp(pk, "yfov") == 0) {
                        if (parse_json_number(&p, &fv)) cams[n].yfov = fv;
                    } else if (strcmp(pk, "znear") == 0) {
                        if (parse_json_number(&p, &fv)) cams[n].znear = fv;
                    } else if (strcmp(pk, "zfar") == 0) {
                        if (parse_json_number(&p, &fv)) cams[n].zfar = fv;
                    } else {
                        skip_value(&p);
                    }
                    skip_ws_ptr(&p);
                    if (*p == ',') p++;
                }
                if (*p == '}') p++;
                obj = p;
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

/* ── Parse the scenes array ──────────────────────────────────── */

static int parse_scenes(const char** j, int* root_nodes, int max_root) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int ri = 0;
    /* We only care about the first scene (index 0). */
    int scene_count = 0;
    while (*cur && *cur != ']' && scene_count < 1) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            if (strcmp(kbuf, "nodes") == 0) {
                const char* arr = obj;
                skip_ws_ptr(&arr);
                if (*arr == '[') arr++;
                while (*arr && *arr != ']' && ri < max_root) {
                    skip_ws_ptr(&arr);
                    if (*arr == ']') break;
                    float fv;
                    if (parse_json_number(&arr, &fv))
                        root_nodes[ri++] = (int)fv;
                    skip_ws_ptr(&arr);
                    if (*arr == ',') arr++;
                }
                if (*arr == ']') arr++;
                obj = arr;
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        scene_count++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return ri;
}

/* ── Parse the materials array ───────────────────────────────── */

static int parse_materials(const char** j, GltfMaterial* mats, int max) {
    const char* cur = *j;
    skip_ws_ptr(&cur);
    if (*cur != '[') return 0;
    cur++;
    int n = 0;
    while (*cur && *cur != ']' && n < max) {
        skip_ws_ptr(&cur);
        if (*cur == ']') break;
        if (*cur != '{') { skip_value(&cur); continue; }
        memset(&mats[n], 0, sizeof(mats[n]));
        mats[n].base_color[0] = 1.0f;
        mats[n].base_color[1] = 1.0f;
        mats[n].base_color[2] = 1.0f;
        mats[n].base_color[3] = 1.0f;
        mats[n].metallic = 1.0f;
        mats[n].ior = 1.5f;

        const char* obj = cur;
        skip_ws_ptr(&obj);
        if (*obj == '{') obj++;
        while (*obj && *obj != '}') {
            char kbuf[128];
            const char* save = obj;
            if (!parse_json_string(&obj, kbuf, sizeof(kbuf))) { obj = save; skip_value(&obj); continue; }
            skip_ws_ptr(&obj);
            if (*obj == ':') obj++;
            skip_ws_ptr(&obj);
            if (strcmp(kbuf, "pbrMetallicRoughness") == 0) {
                const char* p = obj;
                skip_ws_ptr(&p);
                if (*p == '{') p++;
                while (*p && *p != '}') {
                    char pk[64];
                    const char* psave = p;
                    if (!parse_json_string(&p, pk, sizeof(pk))) { p = psave; skip_value(&p); continue; }
                    skip_ws_ptr(&p);
                    if (*p == ':') p++;
                    skip_ws_ptr(&p);
                    if (strcmp(pk, "baseColorFactor") == 0) {
                        parse_float_array(&p, mats[n].base_color, 4);
                    } else if (strcmp(pk, "metallicFactor") == 0) {
                        float fv; if (parse_json_number(&p, &fv)) mats[n].metallic = fv;
                    } else if (strcmp(pk, "roughnessFactor") == 0) {
                        float fv; if (parse_json_number(&p, &fv)) mats[n].roughness = fv;
                    } else {
                        skip_value(&p);
                    }
                    skip_ws_ptr(&p);
                    if (*p == ',') p++;
                }
                if (*p == '}') p++;
                obj = p;
            } else if (strcmp(kbuf, "emissiveFactor") == 0) {
                parse_float_array(&obj, mats[n].emissive, 3);
            } else if (strcmp(kbuf, "extensions") == 0) {
                const char* ex = obj;
                skip_ws_ptr(&ex);
                if (*ex == '{') ex++;
                while (*ex && *ex != '}') {
                    char ek[64];
                    const char* esave = ex;
                    if (!parse_json_string(&ex, ek, sizeof(ek))) { ex = esave; skip_value(&ex); continue; }
                    skip_ws_ptr(&ex);
                    if (*ex == ':') ex++;
                    skip_ws_ptr(&ex);
                    if (strcmp(ek, "KHR_materials_transmission") == 0) {
                        const char* tx = ex;
                        skip_ws_ptr(&tx);
                        if (*tx == '{') tx++;
                        while (*tx && *tx != '}') {
                            char tk[64];
                            const char* tsave = tx;
                            if (!parse_json_string(&tx, tk, sizeof(tk))) { tx = tsave; skip_value(&tx); continue; }
                            skip_ws_ptr(&tx);
                            if (*tx == ':') tx++;
                            skip_ws_ptr(&tx);
                            if (strcmp(tk, "transmissionFactor") == 0) {
                                float fv; if (parse_json_number(&tx, &fv)) mats[n].transmission = fv;
                            } else {
                                skip_value(&tx);
                            }
                            skip_ws_ptr(&tx);
                            if (*tx == ',') tx++;
                        }
                        if (*tx == '}') tx++;
                        ex = tx;
                    } else if (strcmp(ek, "KHR_materials_ior") == 0) {
                        const char* ix = ex;
                        skip_ws_ptr(&ix);
                        if (*ix == '{') ix++;
                        while (*ix && *ix != '}') {
                            char ik[64];
                            const char* isave = ix;
                            if (!parse_json_string(&ix, ik, sizeof(ik))) { ix = isave; skip_value(&ix); continue; }
                            skip_ws_ptr(&ix);
                            if (*ix == ':') ix++;
                            skip_ws_ptr(&ix);
                            if (strcmp(ik, "ior") == 0) {
                                float fv; if (parse_json_number(&ix, &fv)) mats[n].ior = fv;
                            } else {
                                skip_value(&ix);
                            }
                            skip_ws_ptr(&ix);
                            if (*ix == ',') ix++;
                        }
                        if (*ix == '}') ix++;
                        ex = ix;
                    } else {
                        skip_value(&ex);
                    }
                    skip_ws_ptr(&ex);
                    if (*ex == ',') ex++;
                }
                if (*ex == '}') ex++;
                obj = ex;
            } else {
                skip_value(&obj);
            }
            skip_ws_ptr(&obj);
            if (*obj == ',') obj++;
        }
        if (*obj == '}') obj++;
        cur = obj;
        n++;
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    if (*cur == ']') cur++;
    *j = cur;
    return n;
}

/* ── Check for extensionsRequired at the root level ─────────── */

static int check_extensions(const char* json) {
    const char* cur = json;
    skip_ws_ptr(&cur);
    if (*cur != '{') return 0;
    cur++;
    while (*cur && *cur != '}') {
        char kbuf[128];
        const char* save = cur;
        if (!parse_json_string(&cur, kbuf, sizeof(kbuf))) { cur = save; skip_value(&cur); continue; }
        skip_ws_ptr(&cur);
        if (*cur == ':') cur++;
        skip_ws_ptr(&cur);
        if (strcmp(kbuf, "extensionsRequired") == 0) return 1;
        skip_value(&cur);
        skip_ws_ptr(&cur);
        if (*cur == ',') cur++;
    }
    return 0;
}

/* ── Assemble the final GltfScene ──────────────────────────────
 *
 *  Walk the node tree, accumulate transforms, bake them into the
 *  decoded mesh data, and fill the output GltfScene.
 */

static void build_gltf_scene(
    GltfNode* nodes, int nn,
    GltfCamera* cameras, int nc,
    GltfMeshData* meshes, int nm,
    GltfMaterial* materials, int num_materials,
    int* root_nodes, int num_root,
    GltfScene* out)
{
    out->aperture = 0.0f;
    out->focus_dist = 10.0f;

    /* Count max possible MeshObj entries: each (mesh, material) pair. */
    int max_entries = 0;
    for (int i = 0; i < nm; i++)
        for (int p = 0; p < meshes[i].num_prims; p++)
            max_entries++;

    out->num_meshes = 0;
    out->meshes = (MeshObj*)calloc(max_entries > 0 ? max_entries : 1, sizeof(MeshObj));
    if (!out->meshes) return;

    int stack[128];
    float stack_mats[128][16];
    int stack_top = 0;

    for (int ri = 0; ri < num_root; ri++) {
        stack[stack_top] = root_nodes[ri];
        m4_identity(stack_mats[stack_top]);
        stack_top++;
    }


    while (stack_top > 0) {
        stack_top--;
        int ni = stack[stack_top];
        if (ni < 0 || ni >= nn) continue;

        float world[16];
        if (nodes[ni].has_matrix) {
            m4_mul(world, stack_mats[stack_top], nodes[ni].matrix);
        } else {
            float local[16];
            m4_from_trs(local, nodes[ni].translation, nodes[ni].rotation, nodes[ni].scale);
            m4_mul(world, stack_mats[stack_top], local);
        }

        if (nodes[ni].mesh >= 0 && nodes[ni].mesh < nm) {
            GltfMeshData* md = &meshes[nodes[ni].mesh];

            /* Group primitives by material index.  Emit one MeshObj
             * per unique material within this mesh. */
            int used_mat[256];
            int num_used = 0;
            for (int pi = 0; pi < md->num_prims; pi++) {
                int mi = md->prims[pi].material;
                int found = 0;
                for (int u = 0; u < num_used; u++)
                    if (used_mat[u] == mi) { found = 1; break; }
                if (!found) used_mat[num_used++] = mi;
            }

            for (int ui = 0; ui < num_used; ui++) {
                int mat_idx = used_mat[ui];
                MeshObj* mo = &out->meshes[out->num_meshes];
                memset(mo, 0, sizeof(*mo));
                mo->pos = (Vec3){0, 0, 0};
                mo->scale = 1.0f;
                mo->tex_type = 0;
                mo->tex_scale = 1.0f;
                mo->tex_color2 = (Vec3){0, 0, 0};

                /* Look up material properties. */
                float base_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
                float metallic = 0.0f;
                float emissive[3] = {0, 0, 0};
                float transmission = 0.0f;
                float ior = 1.5f;
                if (mat_idx >= 0 && mat_idx < num_materials) {
                    memcpy(base_color, materials[mat_idx].base_color, 4 * sizeof(float));
                    metallic = materials[mat_idx].metallic;
                    memcpy(emissive, materials[mat_idx].emissive, 3 * sizeof(float));
                    transmission = materials[mat_idx].transmission;
                    ior = materials[mat_idx].ior;
                }

                /* Classify material. */
                int is_emissive = (emissive[0] > 0 || emissive[1] > 0 || emissive[2] > 0);
                if (is_emissive) {
                    strcpy(mo->material, "emissive");
                    mo->color = (Vec3){emissive[0], emissive[1], emissive[2]};
                    mo->reflectivity = 0.0f;
                } else if (transmission > 0.0f) {
                    strcpy(mo->material, "glass");
                    mo->color = (Vec3){base_color[0], base_color[1], base_color[2]};
                    mo->reflectivity = 0.0f;
                    mo->ior = ior;
                } else if (metallic > 0.5f) {
                    strcpy(mo->material, "metallic");
                    mo->color = (Vec3){base_color[0], base_color[1], base_color[2]};
                    mo->reflectivity = metallic;
                } else {
                    strcpy(mo->material, "plastic");
                    mo->color = (Vec3){base_color[0], base_color[1], base_color[2]};
                    mo->reflectivity = metallic;
                }

                /* Count triangles for this material group. */
                int total_tris = 0;
                for (int pi = 0; pi < md->num_prims; pi++) {
                    if (md->prims[pi].material != mat_idx) continue;
                    GltfPrimitiveData* pd = &md->prims[pi];
                    if (pd->indices && pd->num_indices >= 3)
                        total_tris += pd->num_indices / 3;
                }

                mo->num_tris = total_tris;
                mo->tris = (TriGpu*)calloc(total_tris > 0 ? total_tris : 1, sizeof(TriGpu));
                if (!mo->tris) { out->num_meshes++; continue; }

                int ti = 0;
                for (int pi = 0; pi < md->num_prims; pi++) {
                    if (md->prims[pi].material != mat_idx) continue;
                    GltfPrimitiveData* pd = &md->prims[pi];
                    if (!pd->positions || !pd->normals || !pd->indices) continue;
                    int num_tris = pd->num_indices / 3;

                    for (int t = 0; t < num_tris && ti < total_tris; t++) {
                        int i0 = pd->indices[t * 3];
                        int i1 = pd->indices[t * 3 + 1];
                        int i2 = pd->indices[t * 3 + 2];

                        float v0[3] = {pd->positions[i0*3], pd->positions[i0*3+1], pd->positions[i0*3+2]};
                        float v1[3] = {pd->positions[i1*3], pd->positions[i1*3+1], pd->positions[i1*3+2]};
                        float v2[3] = {pd->positions[i2*3], pd->positions[i2*3+1], pd->positions[i2*3+2]};
                        float n0[3] = {pd->normals[i0*3], pd->normals[i0*3+1], pd->normals[i0*3+2]};
                        float n1[3] = {pd->normals[i1*3], pd->normals[i1*3+1], pd->normals[i1*3+2]};
                        float n2[3] = {pd->normals[i2*3], pd->normals[i2*3+1], pd->normals[i2*3+2]};

                        m4_transform(v0, world, v0);
                        m4_transform(v1, world, v1);
                        m4_transform(v2, world, v2);
                        m4_transform_normal(n0, world, n0);
                        m4_transform_normal(n1, world, n1);
                        m4_transform_normal(n2, world, n2);

                        for (int k = 0; k < 3; k++) {
                            mo->tris[ti].v0[k] = v0[k];
                            mo->tris[ti].v1[k] = v1[k];
                            mo->tris[ti].v2[k] = v2[k];
                            mo->tris[ti].n0[k] = n0[k];
                            mo->tris[ti].n1[k] = n1[k];
                            mo->tris[ti].n2[k] = n2[k];
                        }
                        mo->tris[ti].t0[0] = mo->tris[ti].t0[1] = 0;
                        mo->tris[ti].t1[0] = mo->tris[ti].t1[1] = 0;
                        mo->tris[ti].t2[0] = mo->tris[ti].t2[1] = 0;
                        mo->tris[ti].mesh_idx = 0;
                        ti++;
                    }
                }
                mo->num_tris = ti;
                out->num_meshes++;
            }
        }

        /* If this node has a camera, extract position & target. */
        if (nodes[ni].camera >= 0 && nodes[ni].camera < nc) {
            /* Position is the translation column of the world matrix. */
            out->camera_pos = (Vec3){world[12], world[13], world[14]};
            /* Forward is -Z column (column 2), transformed by rotation only.
               In a standard glTF camera the forward direction is -Z in local
               space, so we take column 2 (the local Z axis) and negate it. */
            float fwd[3] = {-world[8], -world[9], -world[10]};
            float len = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
            if (len > 1e-8f) {
                fwd[0] /= len; fwd[1] /= len; fwd[2] /= len;
            }
            out->camera_target = (Vec3){
                out->camera_pos.x + fwd[0],
                out->camera_pos.y + fwd[1],
                out->camera_pos.z + fwd[2]
            };
            out->focus_dist = sqrtf(
                fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
            out->aperture = 0.0f;
        }

        /* Push children. */
        for (int ci = nodes[ni].num_children - 1; ci >= 0; ci--) {
            int child = nodes[ni].children[ci];
            if (stack_top < 128) {
                stack[stack_top] = child;
                memcpy(stack_mats[stack_top], world, sizeof(world));
                stack_top++;
            }
        }
    }
}

int load_gltf(const char* path, GltfScene* out) {
    memset(out, 0, sizeof(*out));

    char base_dir[512];
    strncpy(base_dir, path, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    char* last = strrchr(base_dir, '/');
    if (last) *last = '\0'; else { base_dir[0] = '.'; base_dir[1] = '\0'; }

    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* json = (char*)malloc(sz + 1);
    if (!json) { fclose(f); return -1; }
    size_t got = fread(json, 1, sz, f);
    (void)got;
    json[sz] = '\0';
    fclose(f);

    const char* cur = json;
    skip_ws_ptr(&cur);

    GltfBuffer bufs[MAX_BUFFERS];
    GltfBufferView views[MAX_VIEWS];
    GltfAccessor accs[MAX_ACCESSORS];
    GltfMeshRef* mesh_refs = (GltfMeshRef*)calloc(MAX_MESHES, sizeof(GltfMeshRef));
    GltfMeshData* meshes = (GltfMeshData*)calloc(MAX_MESHES, sizeof(GltfMeshData));
    GltfNode nodes[MAX_NODES];
    GltfCamera cameras[MAX_MATERIALS];
    GltfMaterial materials[MAX_MATERIALS];
    int root_nodes[64];
    int nb = 0, nv = 0, na = 0, nm = 0, nn = 0, nc = 0, nmat = 0, nr = 0;

    if (!mesh_refs || !meshes) { free(json); free(mesh_refs); free(meshes); return -1; }
    if (*cur != '{') { free(json); free(mesh_refs); free(meshes); return -1; }

    const char* root = cur;
    skip_ws_ptr(&root);
    if (*root == '{') root++;
    int loop_guard = 0;
    while (*root && *root != '}' && loop_guard < 1000) {
        loop_guard++;
        char kbuf[128];
        const char* save = root;
        if (!parse_json_string(&root, kbuf, sizeof(kbuf))) { root = save; skip_value(&root); continue; }
        skip_ws_ptr(&root);
        if (*root == ':') root++;
        skip_ws_ptr(&root);
        if (strcmp(kbuf, "buffers") == 0) {
            nb = parse_buffers(&root, bufs, MAX_BUFFERS, base_dir);
        } else if (strcmp(kbuf, "bufferViews") == 0) {
            nv = parse_buffer_views(&root, views, MAX_VIEWS);
        } else if (strcmp(kbuf, "accessors") == 0) {
            na = parse_accessors(&root, accs, MAX_ACCESSORS);
        } else if (strcmp(kbuf, "meshes") == 0) {
            nm = parse_mesh_refs(&root, mesh_refs, MAX_MESHES);
        } else if (strcmp(kbuf, "nodes") == 0) {
            nn = parse_nodes(&root, nodes, MAX_NODES);
        } else if (strcmp(kbuf, "cameras") == 0) {
            nc = parse_cameras(&root, cameras, MAX_MATERIALS);
        } else if (strcmp(kbuf, "scene") == 0) {
            float fv; parse_json_number(&root, &fv);
        } else if (strcmp(kbuf, "scenes") == 0) {
            nr = parse_scenes(&root, root_nodes, 64);
        } else if (strcmp(kbuf, "materials") == 0) {
            nmat = parse_materials(&root, materials, MAX_MATERIALS);
        } else {
            skip_value(&root);
        }
        skip_ws_ptr(&root);
        if (*root == ',') root++;
    }
    if (loop_guard >= 1000) { fprintf(stderr, "ERROR: root loop overflow\n"); free(json); free(mesh_refs); free(meshes); return -1; }

    /* Check for extensionsRequired */
    if (check_extensions(json)) {
        fprintf(stderr, "glTF warning: extensionsRequired found — materials using extensions will fall back to plastic\n");
    }

    /* Second pass: decode meshes now that all arrays are loaded */
    decode_meshes(mesh_refs, nm, meshes, accs, na, views, bufs);

    /* Third pass: walk node tree, bake transforms, fill output */
    build_gltf_scene(nodes, nn, cameras, nc, meshes, nm, materials, nmat, root_nodes, nr, out);

    free(json);
    free(mesh_refs);
    free(meshes);

    (void)nb; (void)nv; (void)na; (void)nm; (void)nn; (void)nc; (void)nmat; (void)nr;
    return 0;
}

void free_gltf(GltfScene* out) {
    for (int i = 0; i < out->num_meshes; i++) {
        free(out->meshes[i].tris);
    }
    free(out->meshes);
    memset(out, 0, sizeof(*out));
}