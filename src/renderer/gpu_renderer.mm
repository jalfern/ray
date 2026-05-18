#include "gpu_renderer.h"
#include "bvh.h"
#include "../envmap/envmap.h"
#include <string.h>
#include <math.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

struct SphereGpu {
    float c[3];
    float r;
    float ref;
    float ior;
    float col[3];
    int mat_type;
    int tex_type;
    float tex_scale;
    float tex_color2[3];
};

struct CameraGpu {
    float pos[3];
    float target[3];
    float aperture;
    float focus_dist;
};

struct LightGpu {
    float pos[3];
    float size;
};

struct SceneGpu {
    int num_spheres;
    int num_mesh_tris;
    int num_bvh_nodes;
    int num_meshes;
    int num_lights;
    int num_emissive;
    int num_emissive_cdf;
    float exposure;
    int width;
    int height;
    int has_env;
};

struct MeshMat {
    float col[3];
    float ref;
    float ior;
    int mat_type;
    int tex_type;
    float tex_scale;
    float tex_color2[3];
};

struct EmissiveGpu {
    float emitted[3];
    int type;
    float c[3];
    float r;
    float area;
    int tri_start;
    int tri_end;
    int cdf_offset;
    int src_idx;
};

Image* render_frame_gpu(const Scene* scene) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return NULL;

        NSError* error = nil;
        NSString* path = @"src/renderer/shaders.metal";
        NSString* source = [NSString stringWithContentsOfFile:path
                                                     encoding:NSUTF8StringEncoding
                                                        error:&error];
        if (!source) { fprintf(stderr, "gpu: failed to load shader file\n"); return NULL; }
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                       options:nil
                                                         error:&error];
        if (!library) { fprintf(stderr, "gpu: library compile failed: %s\n", [[error localizedDescription] UTF8String]); return NULL; }

        id<MTLFunction> func = [library newFunctionWithName:@"rk"];
        if (!func) { fprintf(stderr, "gpu: function rk not found\n"); return NULL; }

        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:func error:&error];
        if (!pipeline) { fprintf(stderr, "gpu: pipeline failed: %s\n", [[error localizedDescription] UTF8String]); return NULL; }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) { return NULL; }

        int w = scene->width, h = scene->height;

        id<MTLBuffer> outBuf = [device newBufferWithLength:w * h * 3 * sizeof(float)
                                                   options:MTLResourceStorageModeShared];

        CameraGpu cd;
        cd.pos[0] = scene->camera_pos.x; cd.pos[1] = scene->camera_pos.y; cd.pos[2] = scene->camera_pos.z;
        cd.target[0] = scene->camera_target.x; cd.target[1] = scene->camera_target.y; cd.target[2] = scene->camera_target.z;
        cd.aperture = scene->aperture;
        cd.focus_dist = scene->focus_dist > 0 ? scene->focus_dist : 1.0f;
        id<MTLBuffer> camBuf = [device newBufferWithBytes:&cd length:sizeof(CameraGpu)
                                                  options:MTLResourceStorageModeShared];

        int total_mesh_tris = 0;
        for (int i = 0; i < scene->num_meshes; i++)
            total_mesh_tris += scene->meshes[i].num_tris;

        id<MTLBuffer> triBuf = nil;
        id<MTLBuffer> bvhBuf = nil;
        id<MTLBuffer> matBuf = nil;
        int num_bvh_nodes = 0;

        if (total_mesh_tris > 0) {
            TriGpu* tris = (TriGpu*)malloc(total_mesh_tris * sizeof(TriGpu));
            int ti = 0;
            for (int m = 0; m < scene->num_meshes; m++) {
                for (int j = 0; j < scene->meshes[m].num_tris; j++) {
                    memcpy(&tris[ti], &scene->meshes[m].tris[j], sizeof(TriGpu));
                    tris[ti].mesh_idx = m;
                    ti++;
                }
            }

            int max_nodes = 2 * total_mesh_tris;
            BvhNode* bvh_nodes = (BvhNode*)malloc(max_nodes * sizeof(BvhNode));
            num_bvh_nodes = bvh_build(bvh_nodes, tris, total_mesh_tris);

            triBuf = [device newBufferWithBytes:tris length:total_mesh_tris * sizeof(TriGpu)
                                        options:MTLResourceStorageModeShared];
            bvhBuf = [device newBufferWithBytes:bvh_nodes length:num_bvh_nodes * sizeof(BvhNode)
                                        options:MTLResourceStorageModeShared];
            free(tris);
            free(bvh_nodes);

            MeshMat* mats = (MeshMat*)malloc(scene->num_meshes * sizeof(MeshMat));
            for (int i = 0; i < scene->num_meshes; i++) {
                mats[i].col[0] = scene->meshes[i].color.x;
                mats[i].col[1] = scene->meshes[i].color.y;
                mats[i].col[2] = scene->meshes[i].color.z;
                mats[i].ref = scene->meshes[i].reflectivity;
                mats[i].ior = scene->meshes[i].ior;
                const char* mat = scene->meshes[i].material[0] ? scene->meshes[i].material : "glass";
                int mtype = 0;
                if (strcmp(mat, "plastic") == 0) mtype = 1;
                else if (strcmp(mat, "emissive") == 0) mtype = 2;
                else if (strcmp(mat, "metallic") == 0) mtype = 3;
                else if (strcmp(mat, "subsurface") == 0) mtype = 4;
                mats[i].mat_type = mtype;
                mats[i].tex_type = scene->meshes[i].tex_type;
                mats[i].tex_scale = scene->meshes[i].tex_scale;
                mats[i].tex_color2[0] = scene->meshes[i].tex_color2.x;
                mats[i].tex_color2[1] = scene->meshes[i].tex_color2.y;
                mats[i].tex_color2[2] = scene->meshes[i].tex_color2.z;
            }
            matBuf = [device newBufferWithBytes:mats length:scene->num_meshes * sizeof(MeshMat)
                                        options:MTLResourceStorageModeShared];
            free(mats);
        }

        // Count emissive surfaces
        int num_emissive = 0, cdf_total = 0;
        for (int i = 0; i < scene->num_spheres; i++) {
            const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
            if (strcmp(mat, "emissive") == 0) num_emissive++;
        }
        for (int i = 0; i < scene->num_meshes; i++) {
            const char* mat = scene->meshes[i].material[0] ? scene->meshes[i].material : "glass";
            if (strcmp(mat, "emissive") == 0) {
                num_emissive++;
                cdf_total += scene->meshes[i].num_tris + 1;
            }
        }

        SceneGpu sd;
        sd.num_spheres = scene->num_spheres;
        sd.num_mesh_tris = total_mesh_tris;
        sd.num_bvh_nodes = num_bvh_nodes;
        sd.num_meshes = scene->num_meshes;
        sd.num_lights = scene->num_lights;
        sd.num_emissive = num_emissive;
        sd.num_emissive_cdf = cdf_total;
        sd.exposure = scene->exposure;
        sd.width = w; sd.height = h;
        sd.has_env = (scene->env_file[0] != 0) ? 1 : 0;
        id<MTLBuffer> scBuf = [device newBufferWithBytes:&sd length:sizeof(SceneGpu)
                                                  options:MTLResourceStorageModeShared];

        size_t ss = scene->num_spheres * sizeof(SphereGpu);
        SphereGpu* sp = (SphereGpu*)malloc(ss);
        for (int i = 0; i < scene->num_spheres; i++) {
            sp[i].c[0] = scene->spheres[i].pos.x; sp[i].c[1] = scene->spheres[i].pos.y; sp[i].c[2] = scene->spheres[i].pos.z;
            sp[i].r = scene->spheres[i].radius;
            sp[i].ref = scene->spheres[i].reflectivity;
            sp[i].ior = scene->spheres[i].ior;
            sp[i].col[0] = scene->spheres[i].color.x; sp[i].col[1] = scene->spheres[i].color.y; sp[i].col[2] = scene->spheres[i].color.z;
            const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
            int mtype = 0;
            if (strcmp(mat, "plastic") == 0) mtype = 1;
            else if (strcmp(mat, "emissive") == 0) mtype = 2;
            else if (strcmp(mat, "metallic") == 0) mtype = 3;
            else if (strcmp(mat, "subsurface") == 0) mtype = 4;
            sp[i].mat_type = mtype;
            sp[i].tex_type = scene->spheres[i].tex_type;
            sp[i].tex_scale = scene->spheres[i].tex_scale;
            sp[i].tex_color2[0] = scene->spheres[i].tex_color2.x;
            sp[i].tex_color2[1] = scene->spheres[i].tex_color2.y;
            sp[i].tex_color2[2] = scene->spheres[i].tex_color2.z;
        }
        id<MTLBuffer> sphereBuf = [device newBufferWithBytes:sp length:ss
                                                     options:MTLResourceStorageModeShared];
        free(sp);

        // Light buffer
        id<MTLBuffer> lightBuf = nil;
        if (scene->num_lights > 0) {
            LightGpu* lg = (LightGpu*)malloc(scene->num_lights * sizeof(LightGpu));
            for (int i = 0; i < scene->num_lights; i++) {
                lg[i].pos[0] = scene->lights[i].pos.x;
                lg[i].pos[1] = scene->lights[i].pos.y;
                lg[i].pos[2] = scene->lights[i].pos.z;
                lg[i].size = scene->lights[i].size;
            }
            lightBuf = [device newBufferWithBytes:lg length:scene->num_lights * sizeof(LightGpu)
                                          options:MTLResourceStorageModeShared];
            free(lg);
        }

        // Build emissive surface buffers
        id<MTLBuffer> emissiveBuf = nil;
        id<MTLBuffer> emissiveCdfBuf = nil;

        if (num_emissive > 0) {
            EmissiveGpu* eg = (EmissiveGpu*)malloc(num_emissive * sizeof(EmissiveGpu));
            float* cdf_buf = cdf_total > 0 ? (float*)malloc(cdf_total * sizeof(float)) : NULL;
            int ei = 0, cdf_off = 0, tri_off = 0;

            // Build global tri offset mapping
            for (int m = 0; m < scene->num_meshes; m++) {
                if (strcmp(scene->meshes[m].material[0] ? scene->meshes[m].material : "glass", "emissive") == 0) {
                    // compute later with tri_off
                }
                tri_off += scene->meshes[m].num_tris;
            }

            tri_off = 0;
            for (int m = 0; m < scene->num_meshes; m++) {
                int mesh_emissive = (strcmp(scene->meshes[m].material[0] ? scene->meshes[m].material : "glass", "emissive") == 0);
                if (mesh_emissive) {
                    eg[ei].type = 1;
                    eg[ei].emitted[0] = scene->meshes[m].color.x;
                    eg[ei].emitted[1] = scene->meshes[m].color.y;
                    eg[ei].emitted[2] = scene->meshes[m].color.z;
                    eg[ei].c[0] = eg[ei].c[1] = eg[ei].c[2] = 0;
                    eg[ei].r = 0;
                    eg[ei].tri_start = tri_off;
                    eg[ei].tri_end = tri_off + scene->meshes[m].num_tris;
                    eg[ei].cdf_offset = cdf_off;
                    eg[ei].src_idx = m;
                    float total_area = 0;
                    cdf_buf[cdf_off] = 0;
                    for (int j = 0; j < scene->meshes[m].num_tris; j++) {
                        TriGpu* t = &scene->meshes[m].tris[j];
                        float e1x = t->v1[0] - t->v0[0], e1y = t->v1[1] - t->v0[1], e1z = t->v1[2] - t->v0[2];
                        float e2x = t->v2[0] - t->v0[0], e2y = t->v2[1] - t->v0[1], e2z = t->v2[2] - t->v0[2];
                        float cx = e1y * e2z - e1z * e2y;
                        float cy = e1z * e2x - e1x * e2z;
                        float cz = e1x * e2y - e1y * e2x;
                        total_area += 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);
                        cdf_buf[cdf_off + j + 1] = total_area;
                    }
                    eg[ei].area = total_area;
                    cdf_off += scene->meshes[m].num_tris + 1;
                    ei++;
                }
                tri_off += scene->meshes[m].num_tris;
            }

            for (int i = 0; i < scene->num_spheres; i++) {
                const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
                if (strcmp(mat, "emissive") != 0) continue;
                eg[ei].type = 0;
                eg[ei].emitted[0] = scene->spheres[i].color.x;
                eg[ei].emitted[1] = scene->spheres[i].color.y;
                eg[ei].emitted[2] = scene->spheres[i].color.z;
                eg[ei].c[0] = scene->spheres[i].pos.x;
                eg[ei].c[1] = scene->spheres[i].pos.y;
                eg[ei].c[2] = scene->spheres[i].pos.z;
                eg[ei].r = scene->spheres[i].radius;
                eg[ei].area = 4.0f * (float)M_PI * scene->spheres[i].radius * scene->spheres[i].radius;
                eg[ei].tri_start = eg[ei].tri_end = 0;
                eg[ei].cdf_offset = 0;
                eg[ei].src_idx = i;
                ei++;
            }

            emissiveBuf = [device newBufferWithBytes:eg length:num_emissive * sizeof(EmissiveGpu)
                                              options:MTLResourceStorageModeShared];
            if (cdf_total > 0) {
                emissiveCdfBuf = [device newBufferWithBytes:cdf_buf length:cdf_total * sizeof(float)
                                                     options:MTLResourceStorageModeShared];
            }
            free(eg);
            free(cdf_buf);
        }

        id<MTLTexture> envTex = nil;
        if (scene->env_file[0]) {
            EnvMap* env = envmap_load(scene->env_file, scene->env_intensity);
            if (env) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                    width:env->w height:env->h mipmapped:NO];
                envTex = [device newTextureWithDescriptor:td];
                int n = env->w * env->h;
                float* rgba = (float*)malloc(n * 4 * sizeof(float));
                for (int i = 0; i < n; i++) {
                    rgba[i*4]   = env->data[i*3];
                    rgba[i*4+1] = env->data[i*3+1];
                    rgba[i*4+2] = env->data[i*3+2];
                    rgba[i*4+3] = 1.0f;
                }
                [envTex replaceRegion:MTLRegionMake2D(0, 0, env->w, env->h)
                          mipmapLevel:0 withBytes:rgba bytesPerRow:env->w * 4 * sizeof(float)];
                free(rgba);
                envmap_free(env);
            }
        }

        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:outBuf offset:0 atIndex:0];
        [enc setBuffer:camBuf offset:0 atIndex:1];
        [enc setBuffer:scBuf offset:0 atIndex:2];
        [enc setBuffer:sphereBuf offset:0 atIndex:3];
        [enc setBuffer:triBuf offset:0 atIndex:4];
        [enc setBuffer:bvhBuf offset:0 atIndex:5];
        [enc setBuffer:matBuf offset:0 atIndex:6];
        [enc setBuffer:lightBuf offset:0 atIndex:7];
        [enc setBuffer:emissiveBuf offset:0 atIndex:8];
        [enc setBuffer:emissiveCdfBuf offset:0 atIndex:9];
        if (envTex) [enc setTexture:envTex atIndex:0];

        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tg_w = MIN(16u, pipeline.threadExecutionWidth);
        NSUInteger tg_h = MIN(16u, pipeline.maxTotalThreadsPerThreadgroup / tg_w);
        MTLSize tgSize = MTLSizeMake(tg_w, tg_h, 1);
        [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
        [enc endEncoding];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];

        Image* img = create_image(w, h);
        float* gpu = (float*)outBuf.contents;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int gi = y * w + x;
                size_t ii = (y * w + x) * 3;
                img->data[ii]   = (uint8_t)(fminf(fmaxf(gpu[gi*3],   0.0f), 1.0f) * 255.0f);
                img->data[ii+1] = (uint8_t)(fminf(fmaxf(gpu[gi*3+1], 0.0f), 1.0f) * 255.0f);
                img->data[ii+2] = (uint8_t)(fminf(fmaxf(gpu[gi*3+2], 0.0f), 1.0f) * 255.0f);
            }
        }
        return img;
    }
}
