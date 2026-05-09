// Shading calculations - extracted from renderer.c
#include "shading.h"
#include <math.h>
#include <string.h>

#define EPS 1e-4f

MaterialProps get_material_props(const char* material_name) {
    MaterialProps mat;
    if (strcmp(material_name, "plastic") == 0) {
        mat.specular_strength = 0.4f;
        mat.specular_exponent = 32.0f;
    } else {
        mat.specular_strength = 0.8f;
        mat.specular_exponent = 64.0f;
    }
    return mat;
}

// Floor color pattern (checkboard)
V floor_color(V p) {
    int ix = (int)floorf(p.x);
    int iz = (int)floorf(p.z);
    return ((ix + iz) & 1) ? (V){0.08f, 0.12f, 0.25f} : (V){0.25f, 0.4f, 0.7f};
}
