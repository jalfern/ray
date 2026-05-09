// Generates a torus OBJ: ./gen_torus R r u_segs v_segs > torus.obj
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    float R = 3.0f, r = 1.0f;
    int us = 32, vs = 20;
    if (argc > 1) R = atof(argv[1]);
    if (argc > 2) r = atof(argv[2]);
    if (argc > 3) us = atoi(argv[3]);
    if (argc > 4) vs = atoi(argv[4]);

    printf("# Torus R=%f r=%f u=%d v=%d\n", R, r, us, vs);
    for (int j = 0; j < vs; j++) {
        float v = 2.0f * M_PI * j / vs;
        for (int i = 0; i < us; i++) {
            float u = 2.0f * M_PI * i / us;
            float x = (R + r * cosf(v)) * cosf(u);
            float y = r * sinf(v);
            float z = (R + r * cosf(v)) * sinf(u);
            // Normal
            float nx = cosf(v) * cosf(u);
            float ny = sinf(v);
            float nz = cosf(v) * sinf(u);
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            printf("v %f %f %f\n", x, y, z);
            printf("vt %f %f\n", (float)i / us, (float)j / vs);
            printf("vn %f %f %f\n", nx/len, ny/len, nz/len);
        }
    }
    for (int j = 0; j < vs; j++) {
        for (int i = 0; i < us; i++) {
            int a = j * us + i;
            int b = ((j + 1) % vs) * us + i;
            int c = ((j + 1) % vs) * us + (i + 1) % us;
            int d = j * us + (i + 1) % us;
            printf("f %d/%d/%d %d/%d/%d %d/%d/%d\n", a+1, a+1, a+1, b+1, b+1, b+1, c+1, c+1, c+1);
            printf("f %d/%d/%d %d/%d/%d %d/%d/%d\n", a+1, a+1, a+1, c+1, c+1, c+1, d+1, d+1, d+1);
        }
    }
    return 0;
}
