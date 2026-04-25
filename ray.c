#include <stdio.h>
#include <math.h>
#include <stdint.h>

#define WIDTH 400
#define HEIGHT 400
#define EPS 1e-4f
#define AA_SAMPLES 2  // 2x2 supersampling

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

// Trace a single ray, returns color
V trace_ray(V o, V d) {
 float t_s, t_f;
 int hs = hit_sphere(o, d, (V){0, 2, 0}, 2.0f, &t_s);
 int hf = hit_floor(o, d, &t_f);

 if (hs && (!hf || t_s < t_f)) {
 // Hit sphere
 V p = add(o, mul(d, t_s));
 V n = norm(sub(p, (V){0, 2, 0}));
 // 50% reflective ray
 V refl = sub(d, mul(n, 2.0f * dot(d, n)));
 V rp = add(p, mul(refl, EPS)); // Avoid self-intersection
 V fc = floor_color(rp);
 // 50% reflection + 50% diffuse white base
 return add(mul(fc, 0.5f), mul((V){0.85f,0.85f,0.85f}, 0.5f));
 } else {
 // Hit floor directly
 V p = add(o, mul(d, t_f));
 return floor_color(p);
 }
}

int main() {
 V cam = (V){0, 10, 20};
 V tgt = (V){0, 0, 0};
 V fwd = norm(sub(tgt, cam));
 V right = norm(cross((V){0,1,0}, fwd));
 V up = cross(fwd, right);
 float asp = (float)WIDTH / HEIGHT;

 // Output PPM header
 printf("P6\n%d %d\n255\n", WIDTH, HEIGHT);

 for (int y = 0; y < HEIGHT; y++) {
 for (int x = 0; x < WIDTH; x++) {
 V color_sum = {0, 0, 0};
 int sample_count = 0;

 // 2x2 supersampling
 for (int sy = 0; sy < AA_SAMPLES; sy++) {
 for (int sx = 0; sx < AA_SAMPLES; sx++) {
 // UV coordinates (-1 to 1) with sub-pixel offset
 float uv_x = (2.0f*(x + 0.5f + sx/AA_SAMPLES - 0.5f/AA_SAMPLES)/WIDTH - 1.0f) * asp;
 float uv_y = 1.0f - 2.0f*(y + 0.5f + sy/AA_SAMPLES - 0.5f/AA_SAMPLES)/HEIGHT;
 V ray = norm(add(add(fwd, mul(right, uv_x)), mul(up, uv_y)));

 V color = trace_ray(cam, ray);
 color_sum = add(color_sum, color);
 sample_count++;
 }
 }

 // Average the samples
 V color_avg = mul(color_sum, 1.0f/sample_count);

 // Clamp & convert to 8-bit
 uint8_t cr = (uint8_t)(fminf(fmaxf(color_avg.x, 0.0f), 1.0f) * 255.0f);
 uint8_t cg = (uint8_t)(fminf(fmaxf(color_avg.y, 0.0f), 1.0f) * 255.0f);
 uint8_t cb = (uint8_t)(fminf(fmaxf(color_avg.z, 0.0f), 1.0f) * 255.0f);
 fputc(cr, stdout); fputc(cg, stdout); fputc(cb, stdout);
 }
 }
 return 0;
}
