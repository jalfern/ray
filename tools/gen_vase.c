// Generates a lathed vase OBJ: ./gen_vase segments > vase.obj
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    int segs = 32;
    if (argc > 1) segs = atoi(argv[1]);
    if (segs < 4) segs = 4;

    // Profile curve: (radius, height) pairs
    float profile[][2] = {
        {0.5f, 0.0f},
        {0.6f, 0.3f},
        {1.2f, 0.6f},
        {1.8f, 0.9f},
        {2.0f, 1.2f},
        {1.6f, 1.5f},
        {1.0f, 1.8f},
        {0.8f, 2.0f},
        {0.9f, 2.3f},
        {1.2f, 2.6f},
        {1.0f, 2.8f},
        {0.6f, 3.0f},
        {0.3f, 3.2f},
    };
    int npts = sizeof(profile) / sizeof(profile[0]);
    int nrings = npts;

    printf("# Vase lathe profile=%d points, %d segments\n", npts, segs);

    // Vertices, texture coords, and normals
    for (int j = 0; j < nrings; j++) {
        float r = profile[j][0];
        float y = profile[j][1];
        for (int i = 0; i < segs; i++) {
            float a = 2.0f * M_PI * i / segs;
            float x = r * cosf(a);
            float z = r * sinf(a);
            printf("v %f %f %f\n", x, y, z);
            printf("vt %f %f\n", (float)i / segs, (float)j / (nrings - 1));
            // Approximate normal: radial outward
            float nx = cosf(a);
            float nz = sinf(a);
            printf("vn %f %f %f\n", nx, 0.0f, nz);
        }
    }

    // Faces
    for (int j = 0; j < nrings - 1; j++) {
        for (int i = 0; i < segs; i++) {
            int a = j * segs + i;
            int b = j * segs + (i + 1) % segs;
            int c = (j + 1) * segs + (i + 1) % segs;
            int d = (j + 1) * segs + i;
            printf("f %d/%d/%d %d/%d/%d %d/%d/%d\n", a+1, a+1, a+1, b+1, b+1, b+1, c+1, c+1, c+1);
            printf("f %d/%d/%d %d/%d/%d %d/%d/%d\n", a+1, a+1, a+1, c+1, c+1, c+1, d+1, d+1, d+1);
        }
    }

    return 0;
}
