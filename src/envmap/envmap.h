#ifndef ENVMAP_H
#define ENVMAP_H

typedef struct {
    float* data;  // w * h * 3, row-major RGB
    int w, h;
    float intensity;
} EnvMap;

EnvMap* envmap_load(const char* filename, float intensity);
void envmap_free(EnvMap* env);
void envmap_sample(const EnvMap* env, float dx, float dy, float dz, float* r, float* g, float* b);
void envmap_sample_procedural(float dx, float dy, float dz, float* r, float* g, float* b);

#endif
