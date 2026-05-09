// Generates an icosahedron OBJ: ./gen_ico radius > ico.obj
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    float radius = 2.0f;
    if (argc > 1) radius = atof(argv[1]);

    // Golden ratio
    float t = (1.0f + sqrtf(5.0f)) / 2.0f;
    float verts[12][3] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}
    };
    int faces[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
    };

    printf("# Icosahedron radius=%f\n", radius);
    // Normalize all vertices to sphere of given radius
    for (int i = 0; i < 12; i++) {
        float len = sqrtf(verts[i][0]*verts[i][0] + verts[i][1]*verts[i][1] + verts[i][2]*verts[i][2]);
        float nx = verts[i][0] / len;
        float ny = verts[i][1] / len;
        float nz = verts[i][2] / len;
        printf("v %f %f %f\n", nx * radius, ny * radius, nz * radius);
        float tu = 0.5f + atan2f(nz, nx) / (2.0f * M_PI);
        float tv = 0.5f - asinf(ny) / M_PI;
        printf("vt %f %f\n", tu, tv);
        printf("vn %f %f %f\n", nx, ny, nz);
    }
    for (int i = 0; i < 20; i++) {
        printf("f %d/%d/%d %d/%d/%d %d/%d/%d\n",
               faces[i][0]+1, faces[i][0]+1, faces[i][0]+1,
               faces[i][1]+1, faces[i][1]+1, faces[i][1]+1,
               faces[i][2]+1, faces[i][2]+1, faces[i][2]+1);
    }
    return 0;
}
