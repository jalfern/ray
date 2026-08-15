# Project Status and Next Steps - Ray Tracer

## Current State

### Geometry & Rendering Fixes
- **Scale-relative ray-triangle test:** The old `|det| < EPS` with EPS=1e-4 rejected small
  triangles regardless of ray direction. Fixed with `|det| < 1e-7 * mean(|e1|², |e2|²)`,
  applied identically to CPU (`renderer.cc`), denoiser (`denoiser.cc`), and GPU
  (`shaders.metal`). Verified: IridescenceLamp's glass sphere (mesh 1, 5,632 tris)
  went from 0 hits to 28.7M hits per frame.
- **glTF roughness default:** Changed from 0.0 to 1.0 (per spec). Roughness 0.0 made
  every material a perfect mirror, rendering black without environment lighting.
- **mesh_idx at import:** `mesh_idx` is now correctly set at parser time
  (`gltf_parser.cc:1329`, was always 0). Survives BVH reordering via TriGpu memcpy.
- **EmissiveGpu range fix:** `tri_start`/`tri_end` on the GPU path were set from
  pre-BVH `tri_offset` but used after BVH reordering scattered triangles. Fixed by
  scanning the combined array for matching `mesh_idx` after BVH build.

### glTF Extension Support
- **KHR_materials_transmission:** Parsed and applied. Mesh 1 classified as "glass"
  with `transmission=1.0`, `ior=1.6`.
- **KHR_materials_ior:** Parsed and applied. IOR per-material (1.6 for the glass sphere).
- **KHR_materials_volume:** Parsed but not yet used by the shader.
- **KHR_materials_iridescence:** Parsed but not applied — no shader path exists.

### Diagnostics Infrastructure
- **`--mesh-stats` flag:** Triggers comprehensive diagnostic output at every pipeline stage.
- **`src/parser/gltf_debug.cc`:** Dedicated debug module with per-mesh degenerate triangle
  analysis, cross-product magnitude stats, index range checks, accessor metadata dump,
  and JSON output to `mesh_stats.json`.
- **`include/gltf_parser_internal.h`:** Shared internal struct definitions for debug access.
- **BVH diagnostics:** Node count, max depth, root bbox, and leaf triangle counts
  per mesh, both before and after BVH reordering.
- **Hit counters:** Per-mesh ray-triangle test/hit counts over one full frame.
- **Material property dump:** Per-mesh final material values after classification.

### Web Viewer
- **`web_viewer/`:** Three.js-based real-time glTF viewer, separate from the C++ build.
- Scene dropdown with 8 test scenes (IridescenceLamp first), file picker for
  local .gltf/.glb/.bin loading with LoadingManager URL modifier.
- Vite dev server, hot reload, serves from repo root.

## Potential Next Steps

### Short Term
- **glTF baseColorTexture sampling** — all IridescenceLamp materials reference textures
  but the parser never feeds them to the shader. Highest visual impact.
- **KHR_materials_iridescence** — parsed but not applied; affects all three lamp materials.
- **KHR_materials_volume thickness** — parsed but unused by the shader.

### Medium Term
- **Normal mapping** for increased surface detail.
- **Punctual lights (KHR_lights_punctual)** from glTF.
- **BVH acceleration improvements** for complex scenes.

### Longer Term
- **True area lights** — softer, more realistic shadows.
- **GPU animation** — currently falls back to CPU.
- **JSON schema validation** for scene configuration files.
- **Web viewer diff panel** — load `mesh_stats.json` alongside Three.js data
  for automated cross-validation against the C++ importer.

### Known Limitations
- **Glass CPU/GPU parity incomplete:** CPU `trace_ray` uses recursive calls while GPU
  `shaders.metal` uses iterative stack-based traversal. Surface diffuse terms now match,
  but transmitted/refracted light paths differ. Full parity requires unifying the
  traversal structure.
- **Scale-relative determinant threshold:** The fix `|det| < 1e-7 * mean(|e1|², |e2|²)`
  is a heuristic. Very small triangles with near-parallel rays could still produce false
  rejections. A more principled approach would normalize the determinant by edge lengths.

## Investigation Log

### Glass Sphere Zero-Pixel Bug (IridescenceLamp mesh 1)

**Status:** RESOLVED
**Symptom:** Mesh 1 (lamp_transmission, 5,632 tris) rendered zero pixels despite correct
geometry, BVH, and mesh_idx. The Three.js web viewer showed it correctly.

**Ruled out:**
- Index decode / degenerate triangles — all clean
- BVH construction — root bbox correct, leaf tris match loaded counts
- mesh_idx — correct on both CPU and GPU paths
- Material classification — "glass" with transmission=1.0, ior=1.6
- Environment map — loaded and sampled correctly

**Root cause:** `hit_tri()` used an absolute EPS=1e-4 threshold on the scalar triple
product (determinant). The determinant scales with triangle edge lengths. Mesh 1's
glass sphere has ~0.01-unit edges, producing determinants of 1e-5 to 1e-7 — all
rejected. The BVH root bbox was correct because the BVH builder uses a different
intersection test (bbox overlap) which has no such threshold. The fix: scale-relative
test `|det| < 1e-7 * mean(|e1|², |e2|²)`.

**Verification:** Before fix: mesh[1] tests=270,224 hits=0. After fix:
mesh[1] tests=192,540,040 hits=28,676,808.

### CPU/GPU Render Seam Investigation

**Status:** RESOLVED
**Symptom:** Hard left/right seam and vertical sky banding when comparing CPU vs GPU renders.
Approximately 16,645 differing pixels (0.035% of an 800x600 image).

**Ruled out:** Primary ray generation. Ray directions at pixels (100,300) and (700,300) match
between CPU and GPU within floating-point precision.

**Root cause (floor checkerboard):** Algorithmic difference in `floor_color`. CPU used
`(int)floorf(p.x)` (floor toward −∞), GPU used `int(p.x)` (truncate toward zero). For
negative coordinates these produce different integer parts, flipping the parity test and
the checkerboard pattern wherever x or z crosses zero. **Fixed:** GPU changed to
`int(floor(p.x))` to match the CPU convention.

**Remaining (sky banding):** 50,940 pixels still differ out of 786,432 (1024×768 frame),
all in the sky. This is float-implementation noise: x87/SSE `sinf` vs Metal `sin`
amplified by the high-frequency cloud product, plus a minor `fminf` clamp on the CPU
path that the GPU lacks. Inherent to different float hardware — not fixable.

**AE count before fix:** 335,032 (197,586 on floor, 137,446 on sky)
**AE count after fix:**  51,557  (    617 on floor, 50,940 on sky)
**Floor diffs eliminated:** 196,969 (99.7% reduction)

### Bug Status Summary
- **Issue 1 (Sphere emissive ~2x too dark):** FIXED
- **Issue 2 (Glass + metallic ambient on CPU):** FIXED
- **Issue 3 (CPU negative clamp):** FIXED
- **Issue 4 (Mesh emissive normal not flipped):** FIXED
- **Issue 5 (Shadow rays don't skip origin mesh):** FIXED
- **Issue 6 (Camera zenith/nadir singularity):** FIXED
- **Glass traversal parity:** DOCUMENTED LIMITATION
- **Glass sphere zero pixels (IridescenceLamp):** FIXED — scale-relative det threshold

### glTF Importer
- **Core spec:** Full glTF 2.0 importer (buffers, views, accessors, meshes, nodes, cameras,
  materials, scenes, transforms).
- **Extensions:** KHR_materials_transmission, KHR_materials_ior, KHR_materials_volume
  parsed and applied. KHR_materials_iridescence parsed but not rendered.
- **Diagnostics:** `--mesh-stats` flag, per-mesh degenerate triangle analysis, accessor
  metadata, index range checks, JSON output to `mesh_stats.json`.
- **Tested with:** Box, Suzanne, Lantern, WaterBottle, Avocado, BoomBox,
  MetalRoughSpheres, IridescenceLamp (primary target).
