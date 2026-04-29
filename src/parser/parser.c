#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* skip_ws(char* p) {
    while (*p && isspace(*p)) p++;
    return p;
}

static char* parse_string(char* p, char* buf, int max_len) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            buf[i++] = *p;
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static char* parse_float(char* p, float* val) {
    p = skip_ws(p);
    char* end;
    *val = strtof(p, &end);
    return end;
}

static char* parse_int(char* p, int* val) {
    p = skip_ws(p);
    char* end;
    *val = (int)strtol(p, &end, 10);
    return end;
}

static char* parse_vec3(char* p, Vec3* v) {
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p++;
    p = parse_float(p, &v->x);
    p = skip_ws(p);
    if (*p != ',') return NULL;
    p++;
    p = parse_float(p, &v->y);
    p = skip_ws(p);
    if (*p != ',') return NULL;
    p++;
    p = parse_float(p, &v->z);
    p = skip_ws(p);
    if (*p != ']') return NULL;
    p++;
    return p;
}

static char* parse_sphere(char* p, Sphere* s) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    
    s->color = (Vec3){1.0f, 1.0f, 1.0f};
    strcpy(s->material, "glass");
    
    while (*p && *p != '}') {
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);
        
        if (strcmp(key, "pos") == 0) {
            p = parse_vec3(p, &s->pos);
        } else if (strcmp(key, "radius") == 0) {
            p = parse_float(p, &s->radius);
        } else if (strcmp(key, "reflectivity") == 0) {
            p = parse_float(p, &s->reflectivity);
        } else if (strcmp(key, "color") == 0) {
            p = parse_vec3(p, &s->color);
        } else if (strcmp(key, "material") == 0) {
            p = parse_string(p, s->material, sizeof(s->material));
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == '}') p++;
    return p;
}

static char* parse_spheres_array(char* p, Scene* scene) {
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p++;
    
    scene->num_spheres = 0;
    scene->spheres = NULL;
    
    while (*p && *p != ']') {
        scene->num_spheres++;
        scene->spheres = realloc(scene->spheres, scene->num_spheres * sizeof(Sphere));
        p = parse_sphere(p, &scene->spheres[scene->num_spheres - 1]);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
    return p;
}

Scene* parse_scene(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* json = malloc(size + 1);
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    
    char* p = skip_ws(json);
    if (*p != '{') { free(json); return NULL; }
    p++;
    
    Scene* scene = calloc(1, sizeof(Scene));
    scene->width = 400;
    scene->height = 400;
    scene->has_floor = 0;
    scene->num_spheres = 0;
    scene->spheres = NULL;
    
    while (*p && *p != '}') {
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!key[0]) break;
        
        p = skip_ws(p);
        if (*p != ':') break;
        p++;
        p = skip_ws(p);
        
        if (strcmp(key, "width") == 0) {
            p = parse_int(p, &scene->width);
        } else if (strcmp(key, "height") == 0) {
            p = parse_int(p, &scene->height);
        } else if (strcmp(key, "output") == 0) {
            p = parse_string(p, scene->output, sizeof(scene->output));
        } else if (strcmp(key, "camera") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char ckey[64];
                p = parse_string(p, ckey, sizeof(ckey));
                if (!ckey[0]) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                
                if (strcmp(ckey, "pos") == 0) {
                    p = parse_vec3(p, &scene->camera_pos);
                } else if (strcmp(ckey, "target") == 0) {
                    p = parse_vec3(p, &scene->camera_target);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
                p = skip_ws(p);
                if (*p == ',') p++;
            }
            if (*p == '}') p++;
        } else if (strcmp(key, "light") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char ckey[64];
                p = parse_string(p, ckey, sizeof(ckey));
                if (!ckey[0]) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                
                if (strcmp(ckey, "pos") == 0) {
                    p = parse_vec3(p, &scene->light_pos);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
                p = skip_ws(p);
                if (*p == ',') p++;
            }
            if (*p == '}') p++;
        } else if (strcmp(key, "spheres") == 0) {
            p = parse_spheres_array(p, scene);
        } else if (strcmp(key, "floor") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char ckey[64];
                p = parse_string(p, ckey, sizeof(ckey));
                if (!ckey[0]) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                
                if (strcmp(ckey, "checkerboard") == 0) {
                    scene->has_floor = 1;
                    while (*p && !isspace(*p) && *p != ',' && *p != '}') p++;
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
                p = skip_ws(p);
                if (*p == ',') p++;
            }
            if (*p == '}') p++;
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    
    free(json);
    return scene;
}

void free_scene(Scene* scene) {
    if (scene) {
        free(scene->spheres);
        free(scene);
    }
}
