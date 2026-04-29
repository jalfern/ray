#ifndef SCENE_H
#define SCENE_H

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 pos;
    float radius;
    float reflectivity;
    Vec3 color;
    char material[16];
} Sphere;

typedef struct {
    Vec3 camera_pos;
    Vec3 camera_target;
    Vec3 light_pos;
    Sphere* spheres;
    int num_spheres;
    int width;
    int height;
    int has_floor;
    char output[256];
} Scene;

#endif
