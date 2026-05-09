#ifndef OBJ_PARSER_H
#define OBJ_PARSER_H

#include "../../include/mesh.h"

int load_obj(const char* filename, TriGpu** out_tris, int* out_count);

#endif
