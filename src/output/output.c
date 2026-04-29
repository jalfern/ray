#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define EPS 1e-4f

Image* create_image(int width, int height) {
    Image* img = malloc(sizeof(Image));
    img->width = width;
    img->height = height;
    img->data = calloc(width * height * 3, 1);
    return img;
}

void free_image(Image* img) {
    if (img) {
        free(img->data);
        free(img);
    }
}

int write_ppm(const Image* img, FILE* out) {
    fprintf(out, "P6\n%d %d\n255\n", img->width, img->height);
    fwrite(img->data, 1, img->width * img->height * 3, out);
    return 0;
}

static unsigned long crc32_buf(const unsigned char* buf, size_t len) {
    return crc32(0L, buf, len);
}

static void write_png_chunk(FILE* f, const char* type, const unsigned char* data, size_t len) {
    unsigned long crc = crc32_buf((const unsigned char*)type, 4);
    crc = crc32(crc, data, len);
    
    unsigned char len_buf[4];
    len_buf[0] = (len >> 24) & 0xff;
    len_buf[1] = (len >> 16) & 0xff;
    len_buf[2] = (len >> 8) & 0xff;
    len_buf[3] = len & 0xff;
    
    fwrite(len_buf, 1, 4, f);
    fwrite(type, 1, 4, f);
    fwrite(data, 1, len, f);
    fwrite(&crc, 1, 4, f);
}

int write_png(const Image* img, const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) return -1;
    
    // PNG signature
    unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    
    // IHDR chunk
    unsigned char ihdr_data[13];
    ihdr_data[0] = (img->width >> 24) & 0xff;
    ihdr_data[1] = (img->width >> 16) & 0xff;
    ihdr_data[2] = (img->width >> 8) & 0xff;
    ihdr_data[3] = img->width & 0xff;
    ihdr_data[4] = (img->height >> 24) & 0xff;
    ihdr_data[5] = (img->height >> 16) & 0xff;
    ihdr_data[6] = (img->height >> 8) & 0xff;
    ihdr_data[7] = img->height & 0xff;
    ihdr_data[8] = 8;  // bit depth
    ihdr_data[9] = 2;  // color type (RGB)
    ihdr_data[10] = 0; // compression
    ihdr_data[11] = 0; // filter
    ihdr_data[12] = 0; // interlace
    
    write_png_chunk(f, "IHDR", ihdr_data, 13);
    
    // Compress pixel data with filter byte per row
    size_t raw_size = (img->width * 3 + 1) * img->height;
    unsigned char* raw = malloc(raw_size);
    for (int y = 0; y < img->height; y++) {
        raw[y * (img->width * 3 + 1)] = 0; // None filter
        memcpy(raw + y * (img->width * 3 + 1) + 1,
               img->data + y * img->width * 3,
               img->width * 3);
    }
    
    uLong comp_len = compressBound(raw_size);
    unsigned char* compressed = malloc(comp_len);
    compress2(compressed, &comp_len, raw, raw_size, Z_DEFAULT_COMPRESSION);
    free(raw);
    
    write_png_chunk(f, "IDAT", compressed, comp_len);
    free(compressed);
    
    // IEND chunk
    write_png_chunk(f, "IEND", NULL, 0);
    
    fclose(f);
    return 0;
}
