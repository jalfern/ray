#include <stdio.h>
#include <math.h>
#include <stdint.h>

#define WIDTH 400
#define HEIGHT 400
#define EPS 1e-4f
#define AA_SAMPLES 4  // 4x4 supersampling (16 samples per pixel)

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

// Three shades of blue for checkerboard
V floor_color(V p) {
 int ix = (int)floorf(p.x);
 int iz = (int)floorf(p.z);
 int sum = ix + iz;
 int shade = sum % 3;  // 0, 1, or 2 for three shades
 if (shade == 0) {
  // Darkest blue
  return (V){0.08f, 0.12f, 0.25f};
 } else if (shade == 1) {
  // Medium blue
  return (V){0.25f, 0.4f, 0.7f};
 } else {
  // Light blue
  return (V){0.5f, 0.65f, 0.95f};
 }
}

// Trace a single ray, returns color
V trace_ray(V o, V d, int depth) {
 if (depth > 4) return (V){0,0,0};  // Max reflection depth

 float t_s, t_f;
 int hs = hit_sphere(o, d, (V){0, 2, 0}, 2.0f, &t_s);
 int hf = hit_floor(o, d, &t_f);

 V color;

 if (hs && (!hf || t_s < t_f)) {
  // Hit sphere - mirror-like with specular highlight
  V p = add(o, mul(d, t_s));
  V n = norm(sub(p, (V){0, 2, 0}));
  
  // Light direction (sun in upper right)
  V light = norm((V){1, 2, 1});
  
  // View direction (from camera to hit point)
  V view = norm(sub(o, p));
  
  // Specular highlight (Blinn-Phong)
  V half = norm(add(light, view));
  float spec = powf(fmaxf(0.0f, dot(n, half)), 64.0f);  // High shininess for mirror-like
  
  // 50% reflection ray
  V refl = sub(d, mul(n, 2.0f * dot(d, n)));
  V rp = add(p, mul(refl, EPS));
  
  // Recursive reflection (mirror effect)
  V refl_color = trace_ray(rp, refl, depth + 1);
  
  // Combine: ambient + specular + reflection
  V ambient = (V){0.1f, 0.1f, 0.15f};  // Dark blue-ish ambient
  V spec_color = (V){1.0f, 1.0f, 1.0f};  // White specular highlight
  color = add(ambient, mul(spec_color, spec * 0.8f));  // Specular contribution
  color = add(color, mul(refl_color, 0.7f));  // 70% reflection (mirror-like)
  
 } else if (hf) {
  // Hit floor directly
  V p = add(o, mul(d, t_f));
  color = floor_color(p);
 } else {
  // Miss everything - background
  color = (V){0.05f, 0.05f, 0.1f};  // Dark blue-black
 }

 return color;
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

 // 4x4 supersampling
 for (int sy = 0; sy < AA_SAMPLES; sy++) {
 for (int sx = 0; sx < AA_SAMPLES; sx++) {
 // Sub-pixel position (centered in each sub-pixel)
 float sample_x = (float)(sx + 0.5f) / AA_SAMPLES;
 float sample_y = (float)(sy + 0.5f) / AA_SAMPLES;
 // UV coordinates (-1 to 1) with sub-pixel offset
 float uv_x = (2.0f*(x + sample_x)/WIDTH - 1.0f) * asp;
 float uv_y = 1.0f - 2.0f*(y + sample_y)/HEIGHT;
 V ray = norm(add(add(fwd, mul(right, uv_x)), mul(up, uv_y)));

 V color = trace_ray(cam, ray, 0);
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
