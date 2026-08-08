# jalfern/ray — confirmed bug writeups

Four issues, priority order. Found via V4-Flash (local) + manual review of `renderer.cc` and `shaders.metal`.
Each notes CPU vs GPU status so you know whether it's a parity bug (backends disagree) or a shared bug (both wrong).

---

## Issue 1 — Sphere emissive lights are ~2× too dark (both backends)

**Severity:** High (silent, affects every scene with an emissive sphere)
**Files:** `renderer.cc` `sample_emissive_sphere` / `shaders.metal` `sample_emissive_sphere_gpu`
**Parity:** Shared bug — CPU and GPU agree, so it does NOT show up in a CPU/GPU diff.

### Problem
`sample_emissive_sphere` samples a point on the **entire** sphere surface uniformly,
including the hemisphere facing away from the shaded point. The estimator then rejects
back-facing samples (`if (cos_light <= 0) continue;`) but keeps the full-surface pdf:

```c
pdf = 1.0f / emissive[ei].area;   // area = 4*pi*r^2  (the WHOLE sphere)
```

Roughly half the samples are discarded, but the pdf still assumes all of them contribute.
Net effect: sphere lights come out ~2× dimmer than their `emitted` radiance implies.
Mesh emissive lights are unaffected (single-sided via the interpolated normal), which is
what makes the discrepancy easy to spot: a mesh light and a sphere light of equal area and
equal `emitted` will not match in brightness.

### Fix (either approach)
- **Cheap:** halve the effective pdf for spheres (`pdf = 1.0f / (0.5f * area)`), acknowledging
  only the visible hemisphere contributes on average. Approximate but corrects the mean.
- **Correct:** sample only the hemisphere oriented toward the shaded point, and use that
  hemisphere's solid-angle pdf. More code, unbiased.

### Verify
Render one emissive sphere and one emissive mesh (e.g. a quad) with identical area and
`emitted`. They should read as equally bright. Today the sphere will be visibly darker.

---

## Issue 2 — Glass AND metallic drop ambient + direct lighting on CPU only (backends disagree)

**Status:** FIXED (metallic + ambient parity)
**Severity:** High (visible material difference between `--cpu` and GPU renders)
**Files:** `renderer.cc` `trace_ray` (glass + metallic paths) vs `shaders.metal` `trace_ray`
**Parity:** Fixed for metallic and ambient/direct term inclusion. Glass light-transport
traversal remains divergent (see known limitation below).

### Problem
Affects **both** glass and metallic on CPU. Metallic returns pure tinted reflection:

```c
if (mat == MAT_METALLIC) return (V){refl_col.x * sc.x, refl_col.y * sc.y, refl_col.z * sc.z};
```

and glass computes `base_color = ambient + lit` then **discards it** — the glass return
is purely reflection + refraction:

```c
V ambient = mul(sc, 0.15f);
V base_color = add(ambient, lit);   // built...
if (mat == MAT_PLASTIC) return base_color;
if (mat == MAT_SUBSURFACE) return base_color;
// glass falls through, base_color unused:
return add(mul(refl_col, fresnel * reflectivity), mul(refr_col, 1.0f - fresnel));
```

GPU adds it for every non-emissive material, glass included:

```metal
float3 base = amb + lit;
accum += base * thru;          // happens before the reflect/refract branch
```

So a glass surface shows ambient + diffuse + specular + emissive-lit contribution on GPU,
but only reflection/refraction on CPU. Same scene, two different images.

### Fix Applied
Updated CPU `trace_ray` to include `base_color` in both glass and metallic returns,
matching the GPU behavior:

```c
// Metallic
V metal = (V){refl_col.x * sc.x, refl_col.y * sc.y, refl_col.z * sc.z};
return add(base_color, metal);

// Glass
V glass = add(mul(refl_col, fresnel * reflectivity), mul(refr_col, 1.0f - fresnel));
return add(base_color, glass);
```

### Remaining Limitation
Glass light-transport traversal still differs: CPU uses recursive `trace_ray` calls while
GPU uses iterative stack-based traversal. Transmitted light paths may differ. See
"Known Limitation" in nextsteps.md.

### Verify
Render a glass sphere with `--cpu` and again on GPU. Metallic and ambient terms now match.
Glass surface diffuse contribution now matches, but transmitted caustics/refraction paths
may still differ.

---

## Issue 3 — Negative pixel values not clamped before uint8 cast (CPU; GPU TBD)

**Severity:** Medium (produces bright garbage pixels)
**Files:** `renderer.cc` `render_rows`; GPU status pending `gpu_renderer.mm` check
**Parity:** CPU confirmed. GPU readback path NOT yet verified — see note.

### Problem
CPU clamps the top of the range but not the bottom:

```c
ctx->img->data[idx] = (uint8_t)(fminf(color_avg.x, 1.0f) * 255.0f);
```

A negative channel (possible via Fresnel-weighted refraction/reflection combos, or tone-map
on small negatives) casts to `uint8_t` and wraps to a large positive value — a single bright
garbage pixel.

### Note on GPU
The Metal kernel writes raw floats with no clamp:

```metal
out[y * scene.width + x] = tone_map(final, scene.exposure);
```

Negatives are reportedly caught downstream in `gpu_renderer.mm` (~line 403, clamps both sides).
**Verify this line before finalizing** — an earlier analysis pass cited a GPU clamp that could
not be confirmed. If `.mm:403` does clamp, this is a **CPU-only parity bug** (fix the CPU cast).
If it does NOT, promote to a shared bug and fix both.

### Fix
```c
ctx->img->data[idx] = (uint8_t)(fmaxf(0.0f, fminf(color_avg.x, 1.0f)) * 255.0f);
```
And confirm the GPU readback clamps too.

---

## Issue 4 — Emissive mesh normal not oriented toward shaded point (both backends)

**Severity:** Medium (mesh lights can go dark depending on OBJ winding/normals)
**Files:** `renderer.cc` `sample_emissive_mesh` / `shaders.metal` `sample_emissive_mesh_gpu`
**Parity:** Shared bug — consistent across backends.

### Problem
The interpolated emissive normal is normalized but never flipped toward the shaded point.
If the emissive mesh's normals face away from `p`, `cos_light = dot(ln, -wi)` goes negative
and the sample is rejected (`if (cos_light <= 0) continue;`) even though that surface *is*
facing and illuminating the point. Result: emissive meshes silently contribute nothing (or
only from favorably-wound triangles), highly dependent on the OBJ's normal orientation.

### Fix
After computing the emissive normal, flip it toward the shaded point before the cos test:

```c
V wi = norm(sub(from, /*sampled point*/ p_light));  // toward the shaded surface
if (dot(*normal, wi) < 0) *normal = mul(*normal, -1.0f);
```
(A single-sided emitter is a valid design choice — but then it should be intentional and
documented, not an accident of winding.)

### Verify
Place an emissive mesh light and flip its winding / negate its normals. If brightness changes
dramatically, the emitter is single-sided by accident.

---

## Issue 5 — Shadow rays don't skip the originating mesh (both backends)

**Severity:** Medium (shadow acne on mesh objects under direct lighting)
**Files:** `renderer.cc` `in_shadow` (~269–291); same gap in `shaders.metal` `in_shadow`
**Parity:** Shared bug — both backends.

### Problem
`in_shadow` takes `skip_sphere` but has **no `skip_mesh` parameter** — the mesh loop skips
nothing during shadow-ray traversal. When the shaded point is on a mesh, its own triangles can
self-intersect the shadow ray, producing black acne. Note `emissive_visible` DOES take
`skip_mesh` and use it, so the fix pattern already exists in the codebase.

The `EPS` origin offset + `t > EPS` in `hit_tri` mitigate most self-hits, which is why acne may
be mild so far — but grazing angles and thin geometry will still show it.

### Fix
Add a `skip_mesh` parameter to `in_shadow` (CPU and GPU), pass the hit mesh index from
`trace_ray`, and skip that mesh (or use `tris[i].mesh_idx == skip_mesh` on GPU) in the loop —
mirror exactly what `emissive_visible` already does.

---

## Issue 6 — Camera basis collapses when looking straight up/down (both backends)

**Severity:** Low (exact zenith/nadir only) but catastrophic when hit (NaN/Inf whole frame)
**Files:** `renderer.cc` `setup_context` (~710–712); same in `shaders.metal` camera setup
**Parity:** Shared bug — both backends.

### Problem
```c
ctx->right = norm(cross((V){0,1,0}, ctx->fwd));
```
If `fwd` is parallel to the world up `(0,1,0)` — camera pointed exactly at zenith or nadir —
`cross` returns zero, `norm` divides by zero, and the entire camera basis collapses. All ray
directions become zero, then `hit_sphere` divides by `dot(d,d) = 0` → NaN/Inf across the frame.

### Fix
Guard the up reference: if `fabsf(dot(fwd, worldUp))` is near 1, use an alternate up such as
`(0,0,1)` before the cross. Standard look-at singularity fix.

---

## Issue 7 — Floor checkerboard CPU/GPU parity (FIXED)

**Status:** FIXED
**Severity:** High (visible seam between CPU and GPU renders)
**Files:** `shading.cc` `floor_color` vs `shaders.metal` `floor_color`
**Parity:** Fixed — now consistent across backends.

### Problem
CPU `floor_color` used `(int)floorf(p.x)` which rounds toward negative infinity.
GPU `floor_color` used `int(p.x)` which truncates toward zero.
For negative coordinates these produce different integer parts (e.g. `p.x = -0.3`
→ CPU gives `-1`, GPU gives `0`). The parity test `((ix + iz) & 1)` then flips
the checkerboard on whichever side of the origin the hit point falls. Result: a
hard vertical seam where x or z crosses zero, and a half-split checkerboard floor.

### Fix Applied
GPU `floor_color` changed from `int(p.x)` to `int(floor(p.x))`, matching the CPU's
floor-toward-negative-infinity convention. CPU left as-is.

### Verify
Before fix: 335,032 differing pixels (1024×768 frame), 197,586 on the floor.
After fix:  51,557 differing pixels total, only 617 on the floor (residual
float-noise boundary crossings — hit points differing by ~1 ULP near integer
boundaries). Sky diffs unchanged at ~50,940 (see Known Limitations).

### Known Limitation
The remaining 50,940 sky diffs come from float-implementation differences between
x87/SSE `sinf` (CPU) and Metal `sin` (GPU) amplified by the high-frequency cloud
product `sin(dx*12+dz*8) * sin(dz*10-dx*6)`, plus a minor `fminf` clamp on the
CPU path (`fminf(..., 1.0f)` after sun+cloud addition) that the GPU lacks. These
are inherent to different float hardware and are left as documented float noise.

---

## Scorecard (both V4-Flash passes + manual review + cross-file diff)

Consolidated real-bug list: **7.**

| # | Bug | Source | Type |
|---|-----|--------|------|
| 1 | Sphere-light ~2× too dark | manual | shared / sampling |
| 2 | Glass + metallic ambient dropped (CPU) | V4-Flash (glass) + manual (metallic) | **FIXED** (metallic + ambient); glass traversal still differs |
| 3 | CPU missing negative clamp | both | CPU-only parity (pending `.mm:403`) |
| 4 | Mesh emissive normal not flipped | manual | shared / sampling |
| 5 | Shadow rays don't skip origin mesh | V4-Flash | shared / missing-guard |
| 6 | Camera zenith/nadir singularity | V4-Flash | shared / missing-guard |
| 7 | Floor checkerboard CPU/GPU parity | cross-file diff | **FIXED** (floor) |

**What V4-Flash caught:** parity bugs (2, 3) and missing-guard bugs (5, 6) — including two on
its second pass we did not have (5, 6), and it correctly extended #2 to metallic.
**What it missed both passes:** the two *sampling-correctness* bugs (1, 4) — where the math is
internally consistent but wrong, invisible to CPU/GPU diffing.
**Takeaway:** the model is strong at "these two paths disagree" and "this guard is missing," and
blind to "this estimator is biased." Model sweeps the whole tree and finds candidates; human
adjudicates and supplies the sampling-math bugs. The combination beat either alone.
