#ifndef SHADING_H
#define SHADING_H

#include "../vector/vector.h"
#include "../../include/types.h"

typedef struct {
    float specular_strength;
    float specular_exponent;
} MaterialProps;

V floor_color(V p);
MaterialProps get_material_props(const char* material_name);
V eval_texture(V p, V primary_color, const TextureData* tex);
V eval_texture_uv(V uv, V primary_color, const TextureData* tex);

#endif
