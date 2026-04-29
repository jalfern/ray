#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdio.h>
#include "../include/scene.h"

typedef struct {
    int width;
    int height;
    unsigned char* data;
} Image;

Image* create_image(int width, int height);
void free_image(Image* img);
int write_ppm(const Image* img, FILE* out);
int write_png(const Image* img, const char* filename);

#endif
