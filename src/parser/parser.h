#ifndef PARSER_H
#define PARSER_H

#include "../include/scene.h"

Scene* parse_scene(const char* filename);
void free_scene(Scene* scene);

#endif
