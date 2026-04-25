#include <stdio.h>
#include <math.h>
#include <stdint.h>

#define WIDTH 400
#define HEIGHT 400
#define EPS 1e-4f

typedef struct { float x, y, z; } V;

// Vector operations
V add(V a, V b) { return (V){a.x+b.x, a.y+b.y, a.z+b.z}; }
V sub(V a, V b) { return (V){a.x-b.x, a.y-b.y, a.z-b.z}; }
V mul(V a, float s) { return (V){a.x*s, a.y*s, a.z*s}; }
V norm(V a) { float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); return mul(a, 1.0f/l); }
float dot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V cross(V a, V b) { return (V){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }

// Sphere intersection (center c, radius r)
int hit_sphere(V o, V d, V c, float r, float *t) {
 V oc = sub(o, c);
 float a = dot(d, d);
 float b = 2.0f * dot(oc, d);
 float cc = dot(oc, oc) - r*r;
 float delta = b*b - 4*a*cc;
 if (delta < 0) return 0;
 *t = (-b - sqrtf(delta)) / (2.0f * a);
 return *t > EPS;
}

// Floor (y=0) intersection
int hit_floor(V o, V d, float *t) {
 if (fabsf(d.y) < EPS) return 0;
 *t = -o.y / d.y;
 return *t > EPS;
}

// Checkerboard pattern
V floor_color(V p) {
 int ix = (int)floorf(p.x);
 int iz = (int)floorf(p.z);
 int pat = (ix + iz) & 1;
 return (V){pat ? 0.2f : 0.8f, pat ? 0.2f : 0.8f, pat ? 0.2f : 0.8f};
}

int main() {
 V cam = (V){0, 10, 20};
 V tgt = (V){0, 0, 0};
 V fwd = norm(sub(tgt, cam));
 V right = norm(cross((V){0,1,0}, fwd));
 V up = cross(fwd, right);
 float asp = (float)WIDTH / HEIGHT;

 V sph_c = (V){0, 2, 0}; // Center on floor
 float sph_r = 2.0f;

 // Output PPM header
 printf("P6\n%d %d\n255\n", WIDTH, HEIGHT);

 for (int y = 0; y < HEIGHT; y++) {
 for (int x = 0; x < WIDTH; x++) {
 // UV coordinates (-1 to 1)
 float uv_x = (2.0f*(x+0.5f)/WIDTH - 1.0f) * asp;
 float uv_y = 1.0f - 2.0f*(y+0.5f)/HEIGHT;
 V ray = norm(add(add(fwd, mul(right, uv_x)), mul(up, uv_y)));

 float t_s, t_f;
 int hs = hit_sphere(cam, ray, sph_c, sph_r, &t_s);
 int hf = hit_floor(cam, ray, &t_f);

 V color;
 if (hs && (!hf || t_s < t_f)) {
 // Hit sphere
 V p = add(cam, mul(ray, t_s));
 V n = norm(sub(p, sph_c));
 // 50% reflective ray
 V refl = sub(ray, mul(n, 2.0f * dot(ray, n)));
 V rp = add(p, mul(refl, EPS)); // Avoid self-intersection
 V fc = floor_color(rp);
 // 50% reflection + 50% diffuse white base
 color = add(mul(fc, 0.5f), mul((V){0.85f,0.85f,0.85f}, 0.5f));
 } else {
 // Hit floor directly
 V p = add(cam, mul(ray, t_f));
 color = floor_color(p);
 }

 // Clamp & convert to 8-bit
 uint8_t cr = (uint8_t)(fminf(fmaxf(color.x, 0.0f), 1.0f) * 255.0f);
 uint8_t cg = (uint8_t)(fminf(fmaxf(color.y, 0.0f), 1.0f) * 255.0f);
 uint8_t cb = (uint8_t)(fminf(fmaxf(color.z, 0.0f), 1.0f) * 255.0f);
 fputc(cr, stdout); fputc(cg, stdout); fputc(cb, stdout);
 }
 }
 return 0;
}
