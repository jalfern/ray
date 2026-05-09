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
    int mat_type; // 0=glass (reflective), 1=plastic (matte)
} SphereData;

#endif
