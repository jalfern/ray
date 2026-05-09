#include "gpu_renderer.h"
#include <string.h>
#include <math.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

// Structs must match shaders.metal exactly
struct SphereGpu {
    float c[3];
    float r;
    float ref;
    float col[3];
    int mat_type;
};

struct CameraGpu {
    float pos[3];
    float target[3];
};

struct SceneGpu {
    float light_pos[3];
    int num_spheres;
    int width;
    int height;
};

// Embedded Metal shader source (compiled at runtime)
static const char* shader_src = "#include <metal_stdlib>\n"
"using namespace metal;\n"
"constant float EPS = 1e-4f;\n"
"constant int AA_SAMPLES = 4;\n"
"constant int MAX_DEPTH = 4;\n"
"struct SphereGpu {\n"
"    packed_float3 c; float r; float ref; packed_float3 col; int mat_type;\n"
"};\n"
"struct CameraGpu { packed_float3 pos; packed_float3 target; };\n"
"struct SceneGpu { packed_float3 light_pos; int num_spheres; int width; int height; };\n"
"static bool hit_sphere(float3 o, float3 d, float3 c, float r, thread float& t) {\n"
"    float3 oc = o - c; float a = dot(d,d); float b = 2.0f*dot(oc,d);\n"
"    float cc = dot(oc,oc)-r*r; float delta = b*b-4.0f*a*cc;\n"
"    if (delta < 0.0f) return false; t = (-b - sqrt(delta))/(2.0f*a); return t > EPS;\n"
"}\n"
"static bool hit_any_sphere(float3 o, float3 d, thread float& t, thread float3& n, thread int& idx, device const SphereGpu* s, int c) {\n"
"    float best = 1e9f; bool hit = false; idx = -1;\n"
"    for (int i = 0; i < c; i++) { float ti; if (hit_sphere(o,d,s[i].c,s[i].r,ti) && ti < best) { best = ti; hit = true; idx = i; } }\n"
"    if (hit) { t = best; n = normalize(o + d*best - s[idx].c); } return hit;\n"
"}\n"
"static bool hit_floor(float3 o, float3 d, thread float& t) {\n"
"    if (fabs(d.y) < EPS) return false; t = -o.y/d.y; return t > EPS;\n"
"}\n"
"static float3 floor_color(float3 p) {\n"
"    return ((int(p.x)+int(p.z)) & 1) ? float3(0.08f,0.12f,0.25f) : float3(0.25f,0.4f,0.7f);\n"
"}\n"
"static bool in_shadow(float3 p, float3 lp, device const SphereGpu* s, int c, int origin) {\n"
"    float3 tl = lp-p; float ld = length(tl); float3 rd = normalize(tl); float3 ro = p+rd*EPS;\n"
"    for (int i = 0; i < c; i++) { if (i==origin) continue; float t; if (hit_sphere(ro,rd,s[i].c,s[i].r,t) && t<ld && t>EPS) return true; }\n"
"    return false;\n"
"}\n"
"static float3 trace_ray(float3 o, float3 d, device const SphereGpu* s, int c, float3 lp) {\n"
"    float3 accum = float3(0.0f); float3 thru = float3(1.0f); float3 ro=o, rd=d;\n"
"    for (int depth = 0; depth <= MAX_DEPTH; depth++) {\n"
"        float ts, tf; float3 n; int idx = -1;\n"
"        bool hs = hit_any_sphere(ro, rd, ts, n, idx, s, c);\n"
"        bool hf = hit_floor(ro, rd, tf);\n"
"        if (hs && (!hf || ts < tf)) {\n"
"            float3 p = ro+rd*ts; int plas = (s[idx].mat_type==1); float3 sc = s[idx].col;\n"
"            float3 tl = lp-p; float3 ld = normalize(tl); bool sh = in_shadow(p,lp,s,c,idx);\n"
"            float diff = max(0.0f, dot(n,ld)); float3 vw = normalize(ro-p);\n"
"            float3 hf = normalize(ld+vw); float sp = pow(max(0.0f,dot(n,hf)), plas?32.0f:64.0f);\n"
"            float refl = s[idx].ref; float ss = plas?0.4f:0.8f; float lf = sh?0.0f:1.0f;\n"
"            float3 base = sc*0.15f + sc*diff*lf + sc*sp*ss*lf;\n"
"            accum += base*thru;\n"
"            if (plas || depth==MAX_DEPTH) break;\n"
"            thru *= refl; rd = reflect(rd,n); ro = p+rd*EPS;\n"
"        } else if (hf) {\n"
"            float3 p = ro+rd*tf; float3 nf = float3(0,1,0);\n"
"            float3 tl = lp-p; float3 ld = normalize(tl); bool sh = in_shadow(p,lp,s,c,-1);\n"
"            float3 fl = ((int(p.x)+int(p.z))&1)?float3(0.08f,0.12f,0.25f):float3(0.25f,0.4f,0.7f);\n"
"            float diff = max(0.0f,dot(nf,ld)); float lf = sh?0.2f:1.0f;\n"
"            accum += (fl*0.15f+fl*diff*lf)*thru; break;\n"
"        } else { accum += float3(0.1f,0.1f,0.2f)*thru; break; }\n"
"    }\n"
"    return accum;\n"
"}\n"
"kernel void raytrace_kernel(device float3* out [[buffer(0)]], constant CameraGpu& cam [[buffer(1)]], constant SceneGpu& scene [[buffer(2)]], device const SphereGpu* spheres [[buffer(3)]], uint2 tid [[thread_position_in_grid]], uint2 grid [[threads_per_grid]]) {\n"
"    int x = tid.x, y = tid.y; if (x>=scene.width||y>=scene.height) return;\n"
"    float3 fwd = normalize(cam.target-cam.pos);\n"
"    float3 right = normalize(cross(float3(0,1,0),fwd));\n"
"    float3 up = cross(fwd,right); float asp = float(scene.width)/float(scene.height);\n"
"    float3 sum = float3(0.0f);\n"
"    for (int sy = 0; sy < AA_SAMPLES; sy++) {\n"
"        for (int sx = 0; sx < AA_SAMPLES; sx++) {\n"
"            float ux = (2.0f*(x+(sx+0.5f)/AA_SAMPLES)/scene.width-1.0f)*asp;\n"
"            float uy = 1.0f-2.0f*(y+(sy+0.5f)/AA_SAMPLES)/scene.height;\n"
"            float3 rd = normalize(fwd+right*ux+up*uy);\n"
"            sum += trace_ray(cam.pos,rd,spheres,scene.num_spheres,scene.light_pos);\n"
"        }\n"
"    }\n"
"    out[y*scene.width+x] = sum/float(AA_SAMPLES*AA_SAMPLES);\n"
"}";

Image* render_frame_gpu(const Scene* scene) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return NULL;

        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:shader_src];
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                       options:nil
                                                         error:&error];
        if (!library) {
            fprintf(stderr, "Metal: shader compile failed: %s\n",
                    [[error localizedDescription] UTF8String]);
            return NULL;
        }

        id<MTLFunction> func = [library newFunctionWithName:@"raytrace_kernel"];
        if (!func) {
            fprintf(stderr, "Metal: raytrace_kernel not found in library\n");
            return NULL;
        }

        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:func error:&error];
        if (!pipeline) {
            fprintf(stderr, "Metal: failed to create pipeline: %s\n",
                    [[error localizedDescription] UTF8String]);
            return NULL;
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            fprintf(stderr, "Metal: failed to create command queue\n");
            return NULL;
        }

        // --- Set up buffers ---
        int w = scene->width, h = scene->height;

        // Output buffer (float3 per pixel)
        id<MTLBuffer> outBuf = [device newBufferWithLength:w * h * 3 * sizeof(float)
                                                   options:MTLResourceStorageModeShared];

        // Camera buffer
        CameraGpu cam_data;
        cam_data.pos[0] = scene->camera_pos.x;
        cam_data.pos[1] = scene->camera_pos.y;
        cam_data.pos[2] = scene->camera_pos.z;
        cam_data.target[0] = scene->camera_target.x;
        cam_data.target[1] = scene->camera_target.y;
        cam_data.target[2] = scene->camera_target.z;
        id<MTLBuffer> camBuf = [device newBufferWithBytes:&cam_data
                                                    length:sizeof(CameraGpu)
                                                   options:MTLResourceStorageModeShared];

        // Scene buffer
        SceneGpu sc_data;
        sc_data.light_pos[0] = scene->light_pos.x;
        sc_data.light_pos[1] = scene->light_pos.y;
        sc_data.light_pos[2] = scene->light_pos.z;
        sc_data.num_spheres = scene->num_spheres;
        sc_data.width = w;
        sc_data.height = h;
        id<MTLBuffer> scBuf = [device newBufferWithBytes:&sc_data
                                                  length:sizeof(SceneGpu)
                                                 options:MTLResourceStorageModeShared];

        // Spheres buffer
        size_t sphere_size = scene->num_spheres * sizeof(SphereGpu);
        SphereGpu* sphere_data = (SphereGpu*)malloc(sphere_size);
        for (int i = 0; i < scene->num_spheres; i++) {
            sphere_data[i].c[0] = scene->spheres[i].pos.x;
            sphere_data[i].c[1] = scene->spheres[i].pos.y;
            sphere_data[i].c[2] = scene->spheres[i].pos.z;
            sphere_data[i].r = scene->spheres[i].radius;
            sphere_data[i].ref = scene->spheres[i].reflectivity;
            sphere_data[i].col[0] = scene->spheres[i].color.x;
            sphere_data[i].col[1] = scene->spheres[i].color.y;
            sphere_data[i].col[2] = scene->spheres[i].color.z;
            const char* mat = scene->spheres[i].material[0]
                              ? scene->spheres[i].material : "glass";
            sphere_data[i].mat_type = (strcmp(mat, "plastic") == 0) ? 1 : 0;
        }
        id<MTLBuffer> sphereBuf = [device newBufferWithBytes:sphere_data
                                                      length:sphere_size
                                                     options:MTLResourceStorageModeShared];
        free(sphere_data);

        // --- Dispatch ---
        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:outBuf offset:0 atIndex:0];
        [enc setBuffer:camBuf offset:0 atIndex:1];
        [enc setBuffer:scBuf offset:0 atIndex:2];
        [enc setBuffer:sphereBuf offset:0 atIndex:3];

        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tg_w = MIN(16u, pipeline.threadExecutionWidth);
        NSUInteger tg_h = MIN(16u, pipeline.maxTotalThreadsPerThreadgroup / tg_w);
        MTLSize tgSize = MTLSizeMake(tg_w, tg_h, 1);
        [enc dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
        [enc endEncoding];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];

        // --- Read back ---
        Image* img = create_image(w, h);
        float* gpuData = (float*)outBuf.contents;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int gpu_idx = y * w + x;
                size_t img_idx = (y * w + x) * 3;
                img->data[img_idx]   = (uint8_t)(fminf(fmaxf(gpuData[gpu_idx*3],   0.0f), 1.0f) * 255.0f);
                img->data[img_idx+1] = (uint8_t)(fminf(fmaxf(gpuData[gpu_idx*3+1], 0.0f), 1.0f) * 255.0f);
                img->data[img_idx+2] = (uint8_t)(fminf(fmaxf(gpuData[gpu_idx*3+2], 0.0f), 1.0f) * 255.0f);
            }
        }

        return img;
    }
}
