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
/* Edge-stopping filter in LINEAR radiance space: radiance is w*h*3 floats
   (pre tone_map/encode), filtered in place. */
void denoise(float* radiance, const GBuffer* gbuf, int width, int height, float strength);
void free_gbuffer(GBuffer* gbuf);

#endif
