/* Stage 0 spike (Phase 3, D2): how to make the GPU alpha test bit-exact
 * with the CPU's sample_texture alpha math (renderer.cc:262)?
 *
 * Findings so far (k1/k2, first run):
 *  - texture2d::read EXISTs but takes uint2/ushort2 coords, not int2
 *    (that's why the envtest-fix note recorded read(int2, level) failing).
 *  - read() compiles on plain and array-bound non-mip RGBA8Unorm textures.
 *  - BUT read().a (hardware unorm->float) differs from the CPU's
 *    byte/255.0f by 1 ULP on some values, and the fast-math ON lerp
 *    reassociates (diff ~3e-8). Neither is bit-exact.
 *
 * v2 tests the production candidate: raw RGBA8 bytes in a device buffer,
 * byte/255.0f division + 4-tap lerp mirroring the CPU operation-for-
 * operation, under #pragma METAL fp math_mode(safe) (k3) vs without it
 * (k4, control).
 *
 * v2 RESULT (2026-09-06): k3 == k4 bit-for-bit on every probe UV — the
 * in-body pragma on k3 had NO effect.  math_mode is per-function and the
 * math lives in the alpha_from_buffer HELPER, not in the kernel that
 * carried the pragma.  k4/k3 differ from strict IEEE float32 on 3 of 7
 * UVs (1 ULP, matches an fma-contraction model) — fast-math contraction
 * and/or reciprocal-multiply division are live in the default mode.
 *
 * v3 adds k5: the pragma INSIDE the helper (function scope where the math
 * actually is).  'ieee' mode does not exist in MSL (only fast/relaxed/
 * safe).  If k5 is bit-exact, that is the production shape for MASK alpha.
 *
 * Build:  g++ -O2 -std=c++11 -fobjc-arc tools/spike_alpha_read.mm \
 *             -o /tmp/spike_alpha_read -framework Metal -framework Foundation
 * Run:    /tmp/spike_alpha_read
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* CPU reference: exact mirror of renderer.cc sample_texture, alpha channel. */
static float cpu_alpha_bilinear(const unsigned char* t, int w, int h, float u, float v) {
    u = u - floorf(u);
    v = v - floorf(v);
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + w * 1024) % w;
    int y0 = (iy + h * 1024) % h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;
    float a00 = t[(y0 * w + x0) * 4 + 3] / 255.0f;
    float a10 = t[(y0 * w + x1) * 4 + 3] / 255.0f;
    float a01 = t[(y1 * w + x0) * 4 + 3] / 255.0f;
    float a11 = t[(y1 * w + x1) * 4 + 3] / 255.0f;
    return (1-ry)*((1-rx)*a00 + rx*a10) + ry*((1-rx)*a01 + rx*a11);
}

/* K1: read()-based lerp, plain texture (characterized: ~1 ULP off).
 * K2: read()-based lerp, array-bound texture (TexBundle shape).
 * K3: buffer-based, #pragma METAL fp math_mode(safe) in the KERNEL —
 *     v2 result: identical to K4, pragma never reached the helper's math.
 * K4: buffer-based, no pragma — control.
 * K5: buffer-based, safe pragma INSIDE the helper — production candidate. */
static const char* kShader = R"METAL(
#include <metal_stdlib>
using namespace metal;

kernel void k1(texture2d<float> tex [[texture(0)]],
               constant float2& uv [[buffer(0)]],
               device float* out [[buffer(1)]]) {
    float u = uv.x - floor(uv.x);
    float v = uv.y - floor(uv.y);
    int w = (int)tex.get_width();
    int h = (int)tex.get_height();
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floor(fx);
    int iy = (int)floor(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + w * 1024) % w;
    int y0 = (iy + h * 1024) % h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;
    float a00 = tex.read(uint2(x0, y0), 0).a;
    float a10 = tex.read(uint2(x1, y0), 0).a;
    float a01 = tex.read(uint2(x0, y1), 0).a;
    float a11 = tex.read(uint2(x1, y1), 0).a;
    out[0] = (1-ry)*((1-rx)*a00 + rx*a10) + ry*((1-rx)*a01 + rx*a11);
}

struct Bundle { array<texture2d<float>, 4> t; };

kernel void k2(const device Bundle& b [[buffer(0)]],
               constant float2& uv [[buffer(1)]],
               device float* out [[buffer(2)]]) {
    texture2d<float> tex = b.t[0];
    float u = uv.x - floor(uv.x);
    float v = uv.y - floor(uv.y);
    int w = (int)tex.get_width();
    int h = (int)tex.get_height();
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floor(fx);
    int iy = (int)floor(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + w * 1024) % w;
    int y0 = (iy + h * 1024) % h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;
    float a00 = tex.read(uint2(x0, y0), 0).a;
    float a10 = tex.read(uint2(x1, y0), 0).a;
    float a01 = tex.read(uint2(x0, y1), 0).a;
    float a11 = tex.read(uint2(x1, y1), 0).a;
    out[0] = (1-ry)*((1-rx)*a00 + rx*a10) + ry*((1-rx)*a01 + rx*a11);
}

/* Buffer-based: rgba is the raw RGBA8 image (w*h*4 bytes), exactly the
 * bytes the CPU's ImageTexture.data holds.  Index math, division and lerp
 * mirror renderer.cc sample_texture operation-for-operation. */
static float alpha_from_buffer(device const unsigned char* rgba, int w, int h,
                               float u, float v) {
    u = u - floor(u);
    v = v - floor(v);
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floor(fx);
    int iy = (int)floor(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + w * 1024) % w;
    int y0 = (iy + h * 1024) % h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;
    float a00 = (float)rgba[(y0 * w + x0) * 4 + 3] / 255.0f;
    float a10 = (float)rgba[(y0 * w + x1) * 4 + 3] / 255.0f;
    float a01 = (float)rgba[(y1 * w + x0) * 4 + 3] / 255.0f;
    float a11 = (float)rgba[(y1 * w + x1) * 4 + 3] / 255.0f;
    return (1-ry)*((1-rx)*a00 + rx*a10) + ry*((1-rx)*a01 + rx*a11);
}

/* Same body, pragma INSIDE the function where the math lives. */
static float alpha_from_buffer_safe(device const unsigned char* rgba, int w, int h,
                                    float u, float v) {
    #pragma METAL fp math_mode(safe)
    u = u - floor(u);
    v = v - floor(v);
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floor(fx);
    int iy = (int)floor(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + w * 1024) % w;
    int y0 = (iy + h * 1024) % h;
    int x1 = (x0 + 1) % w;
    int y1 = (y0 + 1) % h;
    float a00 = (float)rgba[(y0 * w + x0) * 4 + 3] / 255.0f;
    float a10 = (float)rgba[(y0 * w + x1) * 4 + 3] / 255.0f;
    float a01 = (float)rgba[(y1 * w + x0) * 4 + 3] / 255.0f;
    float a11 = (float)rgba[(y1 * w + x1) * 4 + 3] / 255.0f;
    return (1-ry)*((1-rx)*a00 + rx*a10) + ry*((1-rx)*a01 + rx*a11);
}

kernel void k3(device const unsigned char* rgba [[buffer(0)]],
               constant int2& wh [[buffer(1)]],
               constant float2& uv [[buffer(2)]],
               device float* out [[buffer(3)]]) {
    #pragma METAL fp math_mode(safe)
    out[0] = alpha_from_buffer(rgba, wh.x, wh.y, uv.x, uv.y);
}

kernel void k4(device const unsigned char* rgba [[buffer(0)]],
               constant int2& wh [[buffer(1)]],
               constant float2& uv [[buffer(2)]],
               device float* out [[buffer(3)]]) {
    out[0] = alpha_from_buffer(rgba, wh.x, wh.y, uv.x, uv.y);
}

kernel void k5(device const unsigned char* rgba [[buffer(0)]],
               constant int2& wh [[buffer(1)]],
               constant float2& uv [[buffer(2)]],
               device float* out [[buffer(3)]]) {
    out[0] = alpha_from_buffer_safe(rgba, wh.x, wh.y, uv.x, uv.y);
}
)METAL";

static int g_fail = 0;

static void check_bits(const char* what, float gpu, float cpu, int enforce) {
    unsigned long long gb, cb;
    memcpy(&gb, &gpu, 4);
    memcpy(&cb, &cpu, 4);
    if (gb == cb) {
        printf("  %-32s GPU=%.9g CPU=%.9g  BIT-EXACT\n", what, (double)gpu, (double)cpu);
    } else {
        printf("  %-32s GPU=%.9g CPU=%.9g  DIFF (dd=%+.3g)%s\n",
               what, (double)gpu, (double)cpu, (double)(gpu - cpu),
               enforce ? "  <-- FAIL" : "  (expected)");
        if (enforce) g_fail = 1;
    }
}

int main() {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("no Metal device\n"); return 2; }
    id<MTLCommandQueue> q = [dev newCommandQueue];

    NSError* lerr = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kShader]
                                            options:nil error:&lerr];
    if (!lib) {
        printf("RESULT: COMPILE FAILED: %s\n",
               lerr ? lerr.localizedDescription.UTF8String : "unknown");
        return 1;
    }
    printf("shader compiled OK (nil options = fast-math ON, same as ray2)\n"); fflush(stdout);

    /* 8x8 RGBA8 texture/buffer: alpha = (x*32 + y*16) & 0xFF, RGB = 200. */
    const int W = 8, H = 8;
    unsigned char px[W * H * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            px[(y * W + x) * 4 + 0] = 200;
            px[(y * W + x) * 4 + 1] = 200;
            px[(y * W + x) * 4 + 2] = 200;
            px[(y * W + x) * 4 + 3] = (unsigned char)((x * 32 + y * 16) & 0xFF);
        }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    MTLRegion region = MTLRegionMake2D(0, 0, W, H);
    [tex replaceRegion:region mipmapLevel:0 withBytes:px bytesPerRow:W * 4];

    id<MTLBuffer> pxBuf = [dev newBufferWithBytes:px length:W * H * 4
                                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> whBuf = [dev newBufferWithLength:2 * sizeof(int)
                                           options:MTLResourceStorageModeShared];
    int wh[2] = {W, H};
    memcpy(whBuf.contents, wh, 2 * sizeof(int));
    id<MTLBuffer> uvBuf = [dev newBufferWithLength:2 * sizeof(float)
                                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> outBuf = [dev newBufferWithLength:8 * sizeof(float)
                                            options:MTLResourceStorageModeShared];

    /* k2's argument encoder: create ONCE (per-UV creation segfaulted). */
    id<MTLFunction> f2 = [lib newFunctionWithName:@"k2"];
    printf("before argenc\n"); fflush(stdout);
    id<MTLArgumentEncoder> ae = [f2 newArgumentEncoderWithBufferIndex:0];
    printf("after argenc %p\n", (__bridge void*)ae); fflush(stdout);
    id<MTLBuffer> abuf = nil;
    if (ae) {
        /* Production order (gpu_renderer.mm:578-583): argument buffer FIRST,
         * then textures. */
        abuf = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
        [ae setArgumentBuffer:abuf offset:0];
        [ae setTexture:tex atIndex:0];
    }
    printf("k2 argenc=%p abuf=%p (encodedLength=%lu)\n",
           (__bridge void*)ae, (__bridge void*)abuf,
           (unsigned long)(ae ? ae.encodedLength : 0));

    const float uvs[][2] = {
        {0.5f / W, 0.5f / H},          /* texel-center: exact, no lerp */
        {0.5f, 0.5f},                  /* mid-quad: full lerp */
        {0.12321f, 0.76543f},          /* arbitrary */
        {0.99999f, 0.00001f},          /* wrap corner */
        {1.25f, -0.25f},               /* out-of-range wrap */
        {0.0f, 0.0f},                  /* corner */
        {0.999999f, 0.999999f},        /* far corner */
    };
    const int NUV = (int)(sizeof(uvs) / sizeof(uvs[0]));

    struct Pass { const char* name; id<MTLFunction> fn; int kind; int enforce; };
    Pass passes[5] = {
        { "k1 read() plain tex",      [lib newFunctionWithName:@"k1"], 1, 0 },
        { "k2 read() array tex",      f2,                               2, 0 },
        { "k3 buffer + safe pragma (kernel)",  [lib newFunctionWithName:@"k3"], 3, 0 },
        { "k4 buffer, no pragma",     [lib newFunctionWithName:@"k4"], 4, 0 },
        { "k5 helper + safe pragma",  [lib newFunctionWithName:@"k5"], 3, 1 },
    };

    for (int p = 0; p < 5; p++) {
        if (!passes[p].fn) { printf("\n%s: function missing\n", passes[p].name); g_fail = 1; continue; }
        NSError* perr = nil;
        id<MTLComputePipelineState> pso =
            [dev newComputePipelineStateWithFunction:passes[p].fn error:&perr];
        if (!pso) { printf("\n%s: pipeline failed: %s\n", passes[p].name,
                           perr ? perr.localizedDescription.UTF8String : "?"); g_fail = 1; continue; }
        printf("\n%s:\n", passes[p].name);
        for (int i = 0; i < NUV; i++) {
            float uvf[2] = {uvs[i][0], uvs[i][1]};
            memcpy(uvBuf.contents, uvf, 2 * sizeof(float));
            memset(outBuf.contents, 0, 8 * sizeof(float));
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            switch (passes[p].kind) {
            case 1:
                [enc setTexture:tex atIndex:0];
                [enc setBuffer:uvBuf offset:0 atIndex:0];
                [enc setBuffer:outBuf offset:0 atIndex:1];
                break;
            case 2:
                [enc setBuffer:abuf offset:0 atIndex:0];
                [enc setBuffer:uvBuf offset:0 atIndex:1];
                [enc setBuffer:outBuf offset:0 atIndex:2];
                break;
            case 3:
            case 4:
                [enc setBuffer:pxBuf offset:0 atIndex:0];
                [enc setBuffer:whBuf offset:0 atIndex:1];
                [enc setBuffer:uvBuf offset:0 atIndex:2];
                [enc setBuffer:outBuf offset:0 atIndex:3];
                break;
            }
            [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) {
                printf("  uv %d: command buffer status %d\n", i, (int)cb.status);
                g_fail = 1;
                continue;
            }
            float* out = (float*)outBuf.contents;
            float cpu = cpu_alpha_bilinear(px, W, H, uvs[i][0], uvs[i][1]);
            char label[64];
            snprintf(label, sizeof(label), "uv=(%.5f, %.5f)", uvs[i][0], uvs[i][1]);
            check_bits(label, out[0], cpu, passes[p].enforce);
        }
    }

    printf("\nRESULT: %s\n",
           g_fail ? "FAIL — helper-scoped pragma not bit-exact"
                  : "PASS — helper-scoped pragma bit-exact; use that path for MASK alpha");
    return g_fail;
}
