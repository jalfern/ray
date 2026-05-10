#ifndef DENOISER_H
#define DENOISER_H

#include "../../include/scene.h"
#include "../output/output.h"

typedef struct {
    float* normal_x;
    float* normal_y;
    float* normal_z;
    float* depth;
    float* albedo_r;
    float* albedo_g;
    float* albedo_b;
} GBuffer;

GBuffer* trace_gbuffer(const Scene* scene);
void denoise(Image* img, const GBuffer* gbuf, int width, int height, float strength);
void free_gbuffer(GBuffer* gbuf);

#endif
