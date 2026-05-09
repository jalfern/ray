#include "parser.h"
#include "obj_parser.h"
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
    s->tex_color2 = (Vec3){0, 0, 0};
    s->ior = 1.5f;
    s->tex_type = 0;
    s->tex_scale = 1.0f;
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
        } else if (strcmp(key, "ior") == 0) {
            p = parse_float(p, &s->ior);
        } else if (strcmp(key, "color") == 0) {
            p = parse_vec3(p, &s->color);
        } else if (strcmp(key, "material") == 0) {
            p = parse_string(p, s->material, sizeof(s->material));
        } else if (strcmp(key, "texture") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char tkey[64];
                p = parse_string(p, tkey, sizeof(tkey));
                if (!p) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                if (strcmp(tkey, "type") == 0) {
                    char buf[16];
                    p = parse_string(p, buf, sizeof(buf));
                    if (strcmp(buf, "checker") == 0) s->tex_type = 1;
                    else if (strcmp(buf, "polka") == 0) s->tex_type = 2;
                    else if (strcmp(buf, "marble") == 0) s->tex_type = 3;
                    else if (strcmp(buf, "rings") == 0) s->tex_type = 4;
                } else if (strcmp(tkey, "scale") == 0) {
                    p = parse_float(p, &s->tex_scale);
                } else if (strcmp(tkey, "color2") == 0) {
                    p = parse_vec3(p, &s->tex_color2);
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
    if (*p == '}') p++;
    return p;
}

static char* parse_light(char* p, Light* l) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    l->size = 0;
    while (*p && *p != '}') {
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);
        if (strcmp(key, "pos") == 0) {
            p = parse_vec3(p, &l->pos);
        } else if (strcmp(key, "size") == 0) {
            p = parse_float(p, &l->size);
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == '}') p++;
    return p;
}

static char* parse_lights_array(char* p, Scene* scene) {
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p++;
    scene->num_lights = 0;
    scene->lights = NULL;
    while (*p && *p != ']') {
        scene->num_lights++;
        scene->lights = (Light*)realloc(scene->lights, scene->num_lights * sizeof(Light));
        p = parse_light(p, &scene->lights[scene->num_lights - 1]);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
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
        scene->spheres = (Sphere*)realloc(scene->spheres, scene->num_spheres * sizeof(Sphere));
        p = parse_sphere(p, &scene->spheres[scene->num_spheres - 1]);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
    return p;
}

static char* parse_mesh(char* p, MeshObj* m, const char* scene_dir) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;

    m->color = (Vec3){1.0f, 1.0f, 1.0f};
    m->tex_color2 = (Vec3){0, 0, 0};
    m->reflectivity = 0.3f;
    m->ior = 1.5f;
    m->scale = 1.0f;
    m->pos = (Vec3){0, 0, 0};
    m->tris = NULL;
    m->num_tris = 0;
    m->tex_type = 0;
    m->tex_scale = 1.0f;
    strcpy(m->material, "glass");

    char file_path[256] = {0};

    while (*p && *p != '}') {
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);

        if (strcmp(key, "file") == 0) {
            char fname[256];
            p = parse_string(p, fname, sizeof(fname));
            if (!p) return NULL;
            if (fname[0] && fname[0] != '/') {
                snprintf(file_path, sizeof(file_path), "%s/%s", scene_dir, fname);
            } else {
                strncpy(file_path, fname, sizeof(file_path) - 1);
            }
        } else if (strcmp(key, "pos") == 0) {
            p = parse_vec3(p, &m->pos);
        } else if (strcmp(key, "scale") == 0) {
            p = parse_float(p, &m->scale);
        } else if (strcmp(key, "reflectivity") == 0) {
            p = parse_float(p, &m->reflectivity);
        } else if (strcmp(key, "ior") == 0) {
            p = parse_float(p, &m->ior);
        } else if (strcmp(key, "color") == 0) {
            p = parse_vec3(p, &m->color);
        } else if (strcmp(key, "material") == 0) {
            p = parse_string(p, m->material, sizeof(m->material));
        } else if (strcmp(key, "texture") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char tkey[64];
                p = parse_string(p, tkey, sizeof(tkey));
                if (!p) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                if (strcmp(tkey, "type") == 0) {
                    char buf[16];
                    p = parse_string(p, buf, sizeof(buf));
                    if (strcmp(buf, "checker") == 0) m->tex_type = 1;
                    else if (strcmp(buf, "polka") == 0) m->tex_type = 2;
                    else if (strcmp(buf, "marble") == 0) m->tex_type = 3;
                    else if (strcmp(buf, "rings") == 0) m->tex_type = 4;
                } else if (strcmp(tkey, "scale") == 0) {
                    p = parse_float(p, &m->tex_scale);
                } else if (strcmp(tkey, "color2") == 0) {
                    p = parse_vec3(p, &m->tex_color2);
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
    if (*p == '}') p++;

    if (file_path[0]) {
        load_obj(file_path, &m->tris, &m->num_tris);
        // Apply transform (pos + scale) to all vertices
        for (int i = 0; i < m->num_tris; i++) {
            for (int k = 0; k < 3; k++) {
                m->tris[i].v0[k] = m->tris[i].v0[k] * m->scale + (&m->pos.x)[k];
                m->tris[i].v1[k] = m->tris[i].v1[k] * m->scale + (&m->pos.x)[k];
                m->tris[i].v2[k] = m->tris[i].v2[k] * m->scale + (&m->pos.x)[k];
            }
        }
    }

    return p;
}

static char* parse_meshes_array(char* p, Scene* scene, const char* scene_dir) {
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p++;

    scene->num_meshes = 0;
    scene->meshes = NULL;

    while (*p && *p != ']') {
        scene->num_meshes++;
        scene->meshes = (MeshObj*)realloc(scene->meshes, scene->num_meshes * sizeof(MeshObj));
        p = parse_mesh(p, &scene->meshes[scene->num_meshes - 1], scene_dir);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
    return p;
}

static void get_scene_dir(const char* filename, char* dir, int max_len) {
    strncpy(dir, filename, max_len - 1);
    dir[max_len - 1] = '\0';
    char* last = strrchr(dir, '/');
    if (last) *last = '\0';
    else { dir[0] = '.'; dir[1] = '\0'; }
}

Scene* parse_scene(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return NULL;

    char scene_dir[256];
    get_scene_dir(filename, scene_dir, sizeof(scene_dir));
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* json = (char*)malloc(size + 1);
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    
    char* p = skip_ws(json);
    if (*p != '{') { free(json); return NULL; }
    p++;
    
    Scene* scene = (Scene*)calloc(1, sizeof(Scene));
    scene->width = 400;
    scene->height = 400;
    scene->exposure = 1.0f;
    scene->has_floor = 0;
    scene->num_spheres = 0;
    scene->spheres = NULL;
    scene->num_meshes = 0;
    scene->meshes = NULL;
    
    while (*p && *p != '}') {
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) break;
        
        p = skip_ws(p);
        if (*p != ':') break;
        p++;
        p = skip_ws(p);
        
        if (strcmp(key, "width") == 0) {
            p = parse_int(p, &scene->width);
        } else if (strcmp(key, "height") == 0) {
            p = parse_int(p, &scene->height);
        } else if (strcmp(key, "exposure") == 0) {
            p = parse_float(p, &scene->exposure);
        } else if (strcmp(key, "output") == 0) {
            p = parse_string(p, scene->output, sizeof(scene->output));
        } else if (strcmp(key, "camera") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char ckey[64];
                p = parse_string(p, ckey, sizeof(ckey));
                if (!p) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);
                
                if (strcmp(ckey, "pos") == 0) {
                    p = parse_vec3(p, &scene->camera_pos);
                } else if (strcmp(ckey, "target") == 0) {
                    p = parse_vec3(p, &scene->camera_target);
                } else if (strcmp(ckey, "aperture") == 0) {
                    p = parse_float(p, &scene->aperture);
                } else if (strcmp(ckey, "focus_dist") == 0) {
                    p = parse_float(p, &scene->focus_dist);
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
                p = skip_ws(p);
                if (*p == ',') p++;
            }
            if (*p == '}') p++;
        } else if (strcmp(key, "lights") == 0) {
            p = parse_lights_array(p, scene);
            if (!p) break;
        } else if (strcmp(key, "spheres") == 0) {
            p = parse_spheres_array(p, scene);
            if (!p) break;
        } else if (strcmp(key, "meshes") == 0) {
            p = parse_meshes_array(p, scene, scene_dir);
            if (!p) break;
        } else if (strcmp(key, "animation") == 0) {
            scene->has_animation = 1;
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char akey[64];
                p = parse_string(p, akey, sizeof(akey));
                if (!p) break;
                p = skip_ws(p);
                if (*p != ':') break;
                p++;
                p = skip_ws(p);

                if (strcmp(akey, "duration") == 0) {
                    p = parse_float(p, &scene->animation.duration);
                } else if (strcmp(akey, "fps") == 0) {
                    p = parse_int(p, &scene->animation.fps);
                } else if (strcmp(akey, "orbit") == 0) {
                    if (*p == '{') p++;
                    while (*p && *p != '}') {
                        char okey[64];
                        p = parse_string(p, okey, sizeof(okey));
                        if (!p) break;
                        p = skip_ws(p);
                        if (*p != ':') break;
                        p++;
                        p = skip_ws(p);

                        if (strcmp(okey, "center") == 0) {
                            p = parse_vec3(p, &scene->animation.orbit_center);
                        } else if (strcmp(okey, "radius") == 0) {
                            p = parse_float(p, &scene->animation.orbit_radius);
                        } else if (strcmp(okey, "height") == 0) {
                            p = parse_float(p, &scene->animation.orbit_height);
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
            if (*p == '}') p++;
        } else if (strcmp(key, "floor") == 0) {
            if (*p == '{') p++;
            while (*p && *p != '}') {
                char ckey[64];
                p = parse_string(p, ckey, sizeof(ckey));
                if (!p) break;
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
    
    // Default light if none specified
    if (scene->num_lights == 0) {
        scene->num_lights = 1;
        scene->lights = (Light*)malloc(sizeof(Light));
        scene->lights[0].pos = (Vec3){5, 10, 5};
        scene->lights[0].size = 0;
    }

    free(json);
    return scene;
}

void free_scene(Scene* scene) {
    if (scene) {
        free(scene->spheres);
        free(scene->lights);
        for (int i = 0; i < scene->num_meshes; i++)
            free(scene->meshes[i].tris);
        free(scene->meshes);
        free(scene);
    }
}
