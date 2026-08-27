//
// Metal GPU ray tracer — host integration.
//
// Feeds the compute kernel in shaders.metal (kernel `rk`). The shader is a
// faithful port of the CPU renderer; this file packs the Scene into GPU
// buffers whose layouts match the structs in shaders.metal exactly, dispatches
// one thread per pixel, and converts the result back into an Image using the
// same 8-bit conversion as the CPU path (renderer.cc render_rows).
//
// The shader/pipeline are cached across calls so they compile only once.
// Returns NULL to fall back to the CPU renderer when Metal is unavailable or
// the shader fails to compile. Full mesh support including BVH, materials,
// emissive meshes, and per-triangle area CDFs.
//
// The shader source is embedded as a string at build time (build/shader_src.h)
// and compiled at runtime via newLibraryWithSource, so no `metal` CLI / Xcode
// is required — only the Metal framework.

#include "gpu_renderer.h"
#include "../../include/types.h"
#include "../../include/bvh.h"
#include "../../include/gltf_debug.h"
#include "../envmap/envmap.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "shader_src.h"    // defines: static const char* kShaderSource

/* Uploads the env (level 0 AND the CPU-built prefilter chain) as a
   mipmapped RGBA32Float texture.  env->data is packed RGB (3 floats/px)
   but the texture is RGBA32Float (4 floats/px): repack each level,
   otherwise replaceRegion misreads every row (bytesPerRow < texture
   row width is an API violation and the sampled image comes out
   sheared/garbage).  Level sizes/chain match the CPU's env->mips
   exactly (box downsample in envmap.cc), so the shader's
   roughness->lod mapping hits the same data the CPU would sample. */
static id<MTLTexture> gpu_create_env_texture(id<MTLDevice> device, EnvMap* env) {
    if (!env || !env->data) return nil;

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                         width:env->w height:env->h mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = (MTLStorageMode)MTLResourceStorageModeShared;
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];
    if (!tex) return nil;
    /* Metal's full chain is max(1,w>>l) x max(1,h>>l) down to 1x1 — the
       same sizes envmap_build_mips produced; guard the assumption so a
       mismatch is loud instead of silently sampling zero-filled levels. */
    if ((int)tex.mipmapLevelCount != env->num_mips) {
        fprintf(stderr, "[gpu] env mip mismatch: texture %lu levels vs CPU %d\n",
                (unsigned long)tex.mipmapLevelCount, env->num_mips);
    }

    int nlevels = env->num_mips < (int)tex.mipmapLevelCount ? env->num_mips : (int)tex.mipmapLevelCount;
    for (int l = 0; l < nlevels; l++) {
        int w = env->mip_w[l], h = env->mip_h[l];
        const float* src = env->mips[l];
        size_t npx = (size_t)w * h;
        float* rgba = (float*)malloc(npx * 4 * sizeof(float));
        for (size_t i = 0; i < npx; i++) {
            rgba[i * 4 + 0] = src[i * 3 + 0];
            rgba[i * 4 + 1] = src[i * 3 + 1];
            rgba[i * 4 + 2] = src[i * 3 + 2];
            rgba[i * 4 + 3] = 1.0f;
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l
                  withBytes:rgba bytesPerRow:(NSUInteger)w * 4 * sizeof(float)];
        free(rgba);
    }
    return tex;
}

// ---------------------------------------------------------------------------
// Host structs — byte-for-byte mirrors of the structs in shaders.metal.
// All members are 4-byte aligned (packed_float3 = 3 floats, no padding), so a
// natural C layout matches Metal's. Sizes are asserted below.
// ---------------------------------------------------------------------------

typedef struct {
    float c[3];
    float r, ref, ior, roughness;
    float col[3];
    int mat_type, tex_type;
    float tex_scale;
    float tex_color2[3];
} SphereGpu;

typedef struct {
    float pos[3];
    float target[3];
    float aperture, focus_dist;
} CameraGpu;

typedef struct {
    float pos[3];
    float size;
} LightGpu;

typedef struct {
    int num_spheres, num_mesh_tris, num_bvh_nodes, num_meshes;
    int num_lights, num_emissive, num_emissive_cdf;
    float exposure;
    int width, height, has_env;
    float fov_scale;
    int num_textures;
    int has_floor, has_bg_color;
    float bg_r, bg_g, bg_b;
    float sh[12];   /* Phase 2 IBL diffuse SH (mirror of SceneGpu.sh) */
    int env_mips;   /* mip-chain length of the env texture (0 = no env) */
} SceneGpu;

typedef struct {
    float emitted[3];
    int type;
    float c[3];
    float r, area;
    int tri_start, tri_end, cdf_offset, src_idx;
} EmissiveGpu;

typedef struct {
    float col[3];
    float ref;
    float ior;
    float roughness;
    float metallic;
    float transmission;
    int mat_type;
    int tex_type;
    float tex_scale;
    float tex_color2[3];
    int tex_index;
    int orm_tex_index;
    int iri_tex_index;
    float iri_factor;
    float iri_ior;
    float iri_thin_min;
    float iri_thin_max;
    float vol_th;
    float att_r;
    float att_g;
    float att_b;
    float att_dist;
    int vol_tex_index;
} MeshMatGpu;

static_assert(sizeof(SphereGpu) == 64, "SphereGpu layout must match shaders.metal");
static_assert(sizeof(CameraGpu) == 32, "CameraGpu layout must match shaders.metal");
static_assert(sizeof(LightGpu) == 16, "LightGpu layout must match shaders.metal");
static_assert(sizeof(SceneGpu) == 124, "SceneGpu layout must match shaders.metal");
static_assert(sizeof(EmissiveGpu) == 52, "EmissiveGpu layout must match shaders.metal");
static_assert(sizeof(MeshMatGpu) == 108, "MeshMatGpu layout must match shaders.metal");

// Cached GPU pipeline — initialized once on first call.
static pthread_mutex_t gpu_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool gpu_initialized = false;
static id<MTLDevice> gpu_device = nil;
static id<MTLComputePipelineState> gpu_pso = nil;
static id<MTLFunction> gpu_fn = nil;
static id<MTLCommandQueue> gpu_queue = nil;

static void gpu_init_once(void) {
    if (gpu_initialized) return;
    pthread_mutex_lock(&gpu_init_mutex);
    if (!gpu_initialized) {
        gpu_device = MTLCreateSystemDefaultDevice();
        if (gpu_device) {
            NSError* err = nil;
            id<MTLLibrary> lib = [gpu_device newLibraryWithSource:@(kShaderSource)
                                                         options:nil
                                                           error:&err];
            if (!lib) {
                fprintf(stderr, "[gpu] shader compile error: %s\n",
                        err ? [[err localizedDescription] UTF8String] : "unknown");
            }
            if (lib) {
                id<MTLFunction> fn = [lib newFunctionWithName:@"rk"];
                if (fn) {
                    gpu_fn = fn;
                    gpu_pso = [gpu_device newComputePipelineStateWithFunction:fn error:&err];
                 }
                if (gpu_pso) {
                    gpu_queue = [gpu_device newCommandQueue];
                 }
              }
            if (!gpu_pso || !gpu_queue) {
                fprintf(stderr, "[gpu] init failed: device=%p pso=%p queue=%p err=%s\n",
                        (__bridge void*)gpu_device, (__bridge void*)gpu_pso,
                        (__bridge void*)gpu_queue,
                        err ? [[err localizedDescription] UTF8String] : "none");
                gpu_device = nil;
             }
         }
        gpu_initialized = true;
     }
    pthread_mutex_unlock(&gpu_init_mutex);
}

#define GPU_GUARD do { \
    gpu_init_once(); \
    if (!gpu_device) return NULL; \
    } while(0)

static int gpu_mat_name_to_type(const char* name) {
    if (strcmp(name, "glass") == 0) return MAT_GLASS;
    if (strcmp(name, "plastic") == 0) return MAT_PLASTIC;
    if (strcmp(name, "emissive") == 0) return MAT_EMISSIVE;
    if (strcmp(name, "metallic") == 0) return MAT_METALLIC;
    if (strcmp(name, "subsurface") == 0) return MAT_SUBSURFACE;
    return MAT_GLASS;
}

static const char* gpu_material(const void* mesh, int is_sphere) {
    if (is_sphere) {
        const Sphere* s = (const Sphere*)mesh;
        return s->material[0] ? s->material : "glass";
      } else {
        const MeshObj* m = (const MeshObj*)mesh;
        return m->material[0] ? m->material : "glass";
      }
}

static float gpu_tri_area(const TriGpu* tri) {
    float e1x = tri->v1[0] - tri->v0[0], e1y = tri->v1[1] - tri->v0[1], e1z = tri->v1[2] - tri->v0[2];
    float e2x = tri->v2[0] - tri->v0[0], e2y = tri->v2[1] - tri->v0[1], e2z = tri->v2[2] - tri->v0[2];
    float cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
    return 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);
}

Image* render_frame_gpu(const Scene* scene) {
    GPU_GUARD;

    const int W = scene->width, H = scene->height;

    @autoreleasepool {
         // --- Camera ---
         CameraGpu cam;
         cam.pos[0] = scene->camera_pos.x; cam.pos[1] = scene->camera_pos.y; cam.pos[2] = scene->camera_pos.z;
         cam.target[0] = scene->camera_target.x; cam.target[1] = scene->camera_target.y; cam.target[2] = scene->camera_target.z;
         cam.aperture = scene->aperture;
         cam.focus_dist = scene->focus_dist > 0 ? scene->focus_dist : 1.0f;

          // --- Spheres ---
         SphereGpu* spheres = (SphereGpu*)calloc(scene->num_spheres > 0 ? scene->num_spheres : 1, sizeof(SphereGpu));
         for (int i = 0; i < scene->num_spheres; i++) {
             const Sphere* s = &scene->spheres[i];
              spheres[i].c[0] = s->pos.x; spheres[i].c[1] = s->pos.y; spheres[i].c[2] = s->pos.z;
             spheres[i].r = s->radius;
             spheres[i].ref = s->reflectivity;
             spheres[i].ior = s->ior;
             spheres[i].roughness = s->roughness;
             spheres[i].col[0] = s->color.x; spheres[i].col[1] = s->color.y; spheres[i].col[2] = s->color.z;
             spheres[i].mat_type = gpu_mat_name_to_type(gpu_material(s, 1));
             spheres[i].tex_type = s->tex_type;
             spheres[i].tex_scale = s->tex_scale;
             spheres[i].tex_color2[0] = s->tex_color2.x; spheres[i].tex_color2[1] = s->tex_color2.y; spheres[i].tex_color2[2] = s->tex_color2.z;
           }

          // --- Lights ---
         LightGpu* lights = (LightGpu*)calloc(scene->num_lights > 0 ? scene->num_lights : 1, sizeof(LightGpu));
         for (int i = 0; i < scene->num_lights; i++) {
             lights[i].pos[0] = scene->lights[i].pos.x;
             lights[i].pos[1] = scene->lights[i].pos.y;
             lights[i].pos[2] = scene->lights[i].pos.z;
             lights[i].size = scene->lights[i].size;
           }

          // --- Meshes: combined triangle array + BVH ---
         int total_tris = 0;
         for (int m = 0; m < scene->num_meshes; m++)
             if (scene->meshes[m].num_tris > 0) total_tris += scene->meshes[m].num_tris;

          TriGpu* all_tris = NULL;
          BvhNode* all_bvh = NULL;
          int num_bvh_nodes = 0;
          int* tri_offset = NULL;
          if (total_tris > 0) {
              all_tris = (TriGpu*)malloc(total_tris * sizeof(TriGpu));
              int off = 0;
              for (int m = 0; m < scene->num_meshes; m++) {
                  for (int t = 0; t < scene->meshes[m].num_tris; t++) {
                      all_tris[off] = scene->meshes[m].tris[t];
                      all_tris[off].mesh_idx = m;
                      off++;
                   }
               }
              /* Compute per-mesh triangle offsets */
              tri_offset = (int*)calloc(scene->num_meshes > 0 ? scene->num_meshes : 1, sizeof(int));
              int acc = 0;
              for (int m = 0; m < scene->num_meshes; m++) {
                  tri_offset[m] = acc;
                  acc += scene->meshes[m].num_tris;
              }
              int max_nodes = 2 * total_tris;
              all_bvh = (BvhNode*)malloc(max_nodes * sizeof(BvhNode));
              if (g_gltf_debug_enabled) {
                  gltf_debug_global_arrays(scene, all_tris, total_tris, tri_offset, NULL, 0, 0);
              }
               num_bvh_nodes = bvh_build(all_bvh, max_nodes, all_tris, total_tris);
               if (g_gltf_debug_enabled) {
                   gltf_debug_global_arrays(scene, all_tris, total_tris, tri_offset, all_bvh, num_bvh_nodes, 1);
                   /* Post-BVH per-mesh_idx count */
                   int* mesh_counts = (int*)calloc(scene->num_meshes > 0 ? scene->num_meshes : 1, sizeof(int));
                   for (int t = 0; t < total_tris; t++) {
                       int midx = all_tris[t].mesh_idx;
                       if (midx >= 0 && midx < scene->num_meshes) mesh_counts[midx]++;
                   }
                   fprintf(stderr, "\n[mesh_idx] Post-BVH triangle counts per mesh_idx:\n");
                   for (int m = 0; m < scene->num_meshes; m++) {
                       fprintf(stderr, "  mesh_idx=%d  count=%d\n", m, mesh_counts[m]);
                   }
                   free(mesh_counts);
               }
             }

          // --- Mesh materials ---
         MeshMatGpu* mats = NULL;
         if (scene->num_meshes > 0) {
             mats = (MeshMatGpu*)calloc(scene->num_meshes, sizeof(MeshMatGpu));
             for (int m = 0; m < scene->num_meshes; m++) {
                 const MeshObj* mo = &scene->meshes[m];
                  mats[m].col[0] = mo->color.x; mats[m].col[1] = mo->color.y; mats[m].col[2] = mo->color.z;
                   mats[m].ref = mo->reflectivity;
                   mats[m].ior = mo->ior;
                    mats[m].roughness = mo->roughness;
                    mats[m].metallic = mo->metallic;
                    mats[m].transmission = mo->transmission;
                  mats[m].tex_index = mo->tex_index;
                  mats[m].orm_tex_index = mo->orm_tex_index;
                  mats[m].iri_tex_index = mo->iri_tex_index;
                   mats[m].iri_factor = mo->iri_factor;
                   mats[m].iri_ior = mo->iri_ior;
                   mats[m].iri_thin_min = mo->iri_thin_min;
                   mats[m].iri_thin_max = mo->iri_thin_max;
                   mats[m].vol_th = mo->vol_th;
                   mats[m].att_r = mo->att_r;
                   mats[m].att_g = mo->att_g;
                   mats[m].att_b = mo->att_b;
                   mats[m].att_dist = mo->att_dist;
                   mats[m].vol_tex_index = mo->vol_tex_index;
                   mats[m].mat_type = gpu_mat_name_to_type(gpu_material(mo, 0));
                 mats[m].tex_type = mo->tex_type;
                 mats[m].tex_scale = mo->tex_scale;
                 mats[m].tex_color2[0] = mo->tex_color2.x; mats[m].tex_color2[1] = mo->tex_color2.y; mats[m].tex_color2[2] = mo->tex_color2.z;
               }
           }

           // --- Emissive surfaces (sphere + mesh) ---
         int num_emissive = 0, num_emissive_cdf = 0;
         for (int i = 0; i < scene->num_spheres; i++)
             if (gpu_mat_name_to_type(gpu_material(&scene->spheres[i], 1)) == MAT_EMISSIVE)
                 num_emissive++;
         for (int i = 0; i < scene->num_meshes; i++)
             if (gpu_mat_name_to_type(gpu_material(&scene->meshes[i], 0)) == MAT_EMISSIVE) {
                 num_emissive++;
                 num_emissive_cdf += scene->meshes[i].num_tris + 1;
                }
         EmissiveGpu* emissive = (EmissiveGpu*)calloc(num_emissive > 0 ? num_emissive : 1, sizeof(EmissiveGpu));
         float* emissive_cdf = (float*)calloc(num_emissive_cdf > 0 ? num_emissive_cdf : 1, sizeof(float));
           {
             int ei = 0, cdf_off = 0;
             for (int i = 0; i < scene->num_spheres; i++) {
                 const Sphere* s = &scene->spheres[i];
                 if (gpu_mat_name_to_type(gpu_material(s, 1)) != MAT_EMISSIVE) continue;
                 emissive[ei].emitted[0] = s->color.x; emissive[ei].emitted[1] = s->color.y; emissive[ei].emitted[2] = s->color.z;
                 emissive[ei].type = 0;
                 emissive[ei].c[0] = s->pos.x; emissive[ei].c[1] = s->pos.y; emissive[ei].c[2] = s->pos.z;
                 emissive[ei].r = s->radius;
                 emissive[ei].area = 4.0f * (float)M_PI * s->radius * s->radius;
                 emissive[ei].tri_start = 0; emissive[ei].tri_end = 0;
                 emissive[ei].cdf_offset = 0; emissive[ei].src_idx = i;
                 ei++;
               }
              for (int i = 0; i < scene->num_meshes; i++) {
                  if (gpu_mat_name_to_type(gpu_material(&scene->meshes[i], 0)) != MAT_EMISSIVE) continue;
                  emissive[ei].emitted[0] = scene->meshes[i].color.x;
                  emissive[ei].emitted[1] = scene->meshes[i].color.y;
                  emissive[ei].emitted[2] = scene->meshes[i].color.z;
                  emissive[ei].type = 1;
                  emissive[ei].c[0] = 0; emissive[ei].c[1] = 0; emissive[ei].c[2] = 0;
                  emissive[ei].r = 0;
                  /* Scan post-BVH combined array for correct range */
                  int s_start = total_tris, s_end = 0;
                  for (int t = 0; t < total_tris; t++) {
                      if (all_tris[t].mesh_idx == i) {
                          if (t < s_start) s_start = t;
                          if (t >= s_end) s_end = t + 1;
                      }
                  }
                  emissive[ei].tri_start = s_start;
                  emissive[ei].tri_end = s_end;
                  emissive[ei].cdf_offset = cdf_off;
                  emissive[ei].src_idx = i;
                  float total = 0;
                  emissive_cdf[cdf_off++] = 0;
                  for (int t = 0; t < scene->meshes[i].num_tris; t++) {
                      total += gpu_tri_area(&scene->meshes[i].tris[t]);
                      emissive_cdf[cdf_off++] = total;
                   }
                  emissive[ei].area = total;
                  ei++;
                }
            }
          free(tri_offset);

          // --- Scene globals ---
         SceneGpu sc;
         sc.num_spheres = scene->num_spheres;
         sc.num_mesh_tris = total_tris;
         sc.num_bvh_nodes = num_bvh_nodes;
         sc.num_meshes = scene->num_meshes;
         sc.num_lights = scene->num_lights;
         sc.num_emissive = num_emissive;
         sc.num_emissive_cdf = num_emissive_cdf;
          sc.exposure = scene->exposure;
          sc.width = W;
          sc.height = H;
           sc.has_env = 0;
           sc.env_mips = 0;
           for (int i = 0; i < 12; i++) sc.sh[i] = 0.0f;
           sc.fov_scale = tanf(scene->fov_y * 0.5f * (float)M_PI / 180.0f);
          sc.num_textures = scene->num_textures;
          sc.has_floor = scene->has_floor;
          sc.has_bg_color = scene->has_bg_color;
          sc.bg_r = scene->bg_color[0];
          sc.bg_g = scene->bg_color[1];
          sc.bg_b = scene->bg_color[2];
          fprintf(stderr, "[gpu] fov_y=%.1f  fov_scale=%.6f  top_uv_y=%.6f  bottom_uv_y=%.6f\n",
                  scene->fov_y, sc.fov_scale,
                  (1.0f - 2.0f * 0.0f / H) * sc.fov_scale,
                  (1.0f - 2.0f * (H - 1) / H) * sc.fov_scale);

          // --- GPU buffers ---
          // Never pass NULL to newBufferWithBytes with Shared storage — Metal will
          // try to read from the pointer and hang silently.
         char dummy[64] = {0};
         size_t tri_len = total_tris > 0 ? (size_t)total_tris * sizeof(TriGpu) : sizeof(dummy);
         TriGpu* tris_ptr = total_tris > 0 ? all_tris : (TriGpu*)dummy;
         size_t bvh_len = num_bvh_nodes > 0 ? (size_t)num_bvh_nodes * sizeof(BvhNode) : sizeof(dummy);
         BvhNode* bvh_ptr = num_bvh_nodes > 0 ? all_bvh : (BvhNode*)dummy;
         size_t mat_len = scene->num_meshes > 0 ? (size_t)scene->num_meshes * sizeof(MeshMatGpu) : sizeof(dummy);
         MeshMatGpu* mats_ptr = scene->num_meshes > 0 ? mats : (MeshMatGpu*)dummy;
         size_t cdf_len = num_emissive_cdf > 0 ? (size_t)num_emissive_cdf * sizeof(float) : sizeof(dummy);
         float* cdf_ptr = num_emissive_cdf > 0 ? emissive_cdf : (float*)dummy;

         const MTLResourceOptions opts = MTLResourceStorageModeShared;
         id<MTLBuffer> outBuf = [gpu_device newBufferWithLength:(NSUInteger)W * H * 3 * sizeof(float) options:opts];
         id<MTLBuffer> sphereBuf = [gpu_device newBufferWithBytes:spheres length:(scene->num_spheres > 0 ? scene->num_spheres : 1) * sizeof(SphereGpu) options:opts];
         id<MTLBuffer> lightBuf = [gpu_device newBufferWithBytes:lights length:(scene->num_lights > 0 ? scene->num_lights : 1) * sizeof(LightGpu) options:opts];
         id<MTLBuffer> emisBuf = [gpu_device newBufferWithBytes:emissive length:(num_emissive > 0 ? num_emissive : 1) * sizeof(EmissiveGpu) options:opts];
         id<MTLBuffer> triBuf = [gpu_device newBufferWithBytes:tris_ptr length:tri_len options:opts];
         id<MTLBuffer> bvhBuf = [gpu_device newBufferWithBytes:bvh_ptr length:bvh_len options:opts];
         id<MTLBuffer> matBuf = [gpu_device newBufferWithBytes:mats_ptr length:mat_len options:opts];
         id<MTLBuffer> cdfBuf = [gpu_device newBufferWithBytes:cdf_ptr length:cdf_len options:opts];

           // Real env texture from HDR file, or dummy 1x1 placeholder.
           // One load feeds both the mip-mapped texture and the IBL SH
           // coefficients in SceneGpu (the CPU renderer loads the same
           // file through the same envmap_load, so SH values and mip
           // chains are bit-identical across backends).
           id<MTLTexture> envTex = nil;
           {
               EnvMap* env = scene->env_file[0] ? envmap_load(scene->env_file, scene->env_intensity) : NULL;
               if (env && env->data) {
                   envTex = gpu_create_env_texture(gpu_device, env);
                   if (envTex) {
                       sc.has_env = 1;
                       sc.env_mips = env->num_mips;
                       for (int i = 0; i < 12; i++) sc.sh[i] = env->sh[i];
                   }
               }
               envmap_free(env);
           }
           if (!envTex) {
               MTLTextureDescriptor* td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                width:1 height:1 mipmapped:NO];
               td.usage = MTLTextureUsageShaderRead;
               envTex = [gpu_device newTextureWithDescriptor:td];
               sc.has_env = 0;
           }

          // --- Scene textures: bind ALL of them as an argument-buffer texture
          //     array so the kernel samples textures[tex_index / orm_tex_index /
          //     iri_tex_index] per material, mirroring the CPU.  Slots without a
          //     usable image fall back to a shared 1x1 dummy (never sampled for a
          //     material whose index is -1, since those take the procedural path). ---
          const int RAY_MAXTEX = 64;   /* must match #define MAXTEX in shaders.metal */
          if (scene->num_textures > RAY_MAXTEX)
              fprintf(stderr, "[gpu] WARNING: %d scene textures exceed MAXTEX(%d); extras ignored\n",
                      scene->num_textures, RAY_MAXTEX);
          id<MTLTexture> texArr[RAY_MAXTEX];
          {
              MTLTextureDescriptor* dd =
                  [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                    width:1 height:1 mipmapped:NO];
              dd.usage = MTLTextureUsageShaderRead;
              dd.storageMode = MTLStorageModeShared;
              id<MTLTexture> dummyT = [gpu_device newTextureWithDescriptor:dd];
              for (int i = 0; i < RAY_MAXTEX; i++) texArr[i] = dummyT;
          }
          for (int i = 0; i < scene->num_textures && i < RAY_MAXTEX; i++) {
              ImageTexture* it = &scene->textures[i];
              if (it->data && it->width > 0 && it->height > 0) {
                  MTLTextureDescriptor* td =
                      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                        width:it->width height:it->height mipmapped:NO];
                  td.usage = MTLTextureUsageShaderRead;
                  td.storageMode = MTLStorageModeShared;
                  id<MTLTexture> t = [gpu_device newTextureWithDescriptor:td];
                  if (t) {
                      MTLRegion region = MTLRegionMake2D(0, 0, it->width, it->height);
                      [t replaceRegion:region mipmapLevel:0 withBytes:it->data bytesPerRow:it->width * 4];
                      texArr[i] = t;
                  }
              }
          }
          id<MTLBuffer> texArgBuf = nil;
          {
              id<MTLArgumentEncoder> argEnc = [gpu_fn newArgumentEncoderWithBufferIndex:10];
              if (argEnc) {
                  texArgBuf = [gpu_device newBufferWithLength:argEnc.encodedLength options:opts];
                  [argEnc setArgumentBuffer:texArgBuf offset:0];
                  /* TexBundle's array<texture2d<float>, MAXTEX> is laid out INLINE
                     as MAXTEX consecutive texture bindings in this argument buffer
                     (encodedLength = MAXTEX * 8), so set each slot directly. */
                  for (int i = 0; i < RAY_MAXTEX; i++) [argEnc setTexture:texArr[i] atIndex:i];
              } else {
                  fprintf(stderr, "[gpu] FATAL: no argument encoder for texture array (buffer 10)\n");
              }
          }

           free(spheres); free(lights); free(emissive); free(mats); free(emissive_cdf);
          free(all_tris); free(all_bvh);

          // --- Dispatch ---
         id<MTLCommandBuffer> cb = [gpu_queue commandBuffer];
         id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
         [enc setComputePipelineState:gpu_pso];
         [enc setBuffer:outBuf offset:0 atIndex:0];
         [enc setBytes:&cam length:sizeof(cam) atIndex:1];
         [enc setBytes:&sc length:sizeof(sc) atIndex:2];
         [enc setBuffer:sphereBuf offset:0 atIndex:3];
         [enc setBuffer:triBuf offset:0 atIndex:4];
         [enc setBuffer:bvhBuf offset:0 atIndex:5];
         [enc setBuffer:matBuf offset:0 atIndex:6];
         [enc setBuffer:lightBuf offset:0 atIndex:7];
         [enc setBuffer:emisBuf offset:0 atIndex:8];
         [enc setBuffer:cdfBuf offset:0 atIndex:9];
            [enc setTexture:envTex atIndex:0];
            if (texArgBuf) [enc setBuffer:texArgBuf offset:0 atIndex:10];

         MTLSize tg = MTLSizeMake(16, 16, 1);
         MTLSize grid = MTLSizeMake(W, H, 1);
         [enc dispatchThreads:grid threadsPerThreadgroup:tg];
         [enc endEncoding];
         [cb commit];
         [cb waitUntilCompleted];

         if (cb.status != MTLCommandBufferStatusCompleted) {
             fprintf(stderr, "[gpu] command buffer failed (status %ld)\n", (long)cb.status);
             return NULL;
           }

          // --- Read back ---
         Image* img = create_image(W, H);
         if (!img) return NULL;
         const float* out = (const float*)outBuf.contents;
         for (int i = 0; i < W * H * 3; i++) {
             float v = out[i];
             if (v < 0.0f) v = 0.0f;
             img->data[i] = (unsigned char)(fminf(v, 1.0f) * 255.0f);
           }
         return img;
       }
}