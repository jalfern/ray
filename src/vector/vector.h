#ifndef VECTOR_H
#define VECTOR_H

typedef struct {
    float x, y, z;
} V;

V add(V a, V b);
V sub(V a, V b);
V mul(V a, float s);
V norm(V a);
float dot(V a, V b);
V cross(V a, V b);

#endif
