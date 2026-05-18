#ifndef PARSER_H
#define PARSER_H

#include "scene.h"
#include "torus.h"

Scene* parse_scene(const char* filename);
void free_scene(Scene* scene);

#endif