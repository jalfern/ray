#ifndef TYPES_H
#define TYPES_H

#include "vector.h"
#include "scene.h"

// Internal renderer types (not exposed in public API)
typedef struct {
    V c;          // center
    float r;      // radius
    float ref;    // reflectivity
    V col;        // color
    char mat[16]; // material name
} SphereData;

#endif
