#include "vector.h"
#include <math.h>

#define EPS 1e-4f

V add(V a, V b) { return (V){a.x+b.x, a.y+b.y, a.z+b.z}; }
V sub(V a, V b) { return (V){a.x-b.x, a.y-b.y, a.z-b.z}; }
V mul(V a, float s) { return (V){a.x*s, a.y*s, a.z*s}; }

V norm(V a) { 
    float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); 
    return (l > EPS) ? mul(a, 1.0f/l) : (V){0,0,0}; 
}

float dot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

V cross(V a, V b) { 
    return (V){
        a.y*b.z - a.z*b.y, 
        a.z*b.x - a.x*b.z, 
        a.x*b.y - a.y*b.x
    }; 
}
