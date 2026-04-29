#ifndef SHADING_H
#define SHADING_H

#include "../vector/vector.h"

typedef struct {
    float specular_strength;
    float specular_exponent;
} MaterialProps;

// Extracted utilities
V floor_color(V p);
MaterialProps get_material_props(const char* material_name);

#endif
