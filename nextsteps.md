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
  (`gltf_parser.cc:1562`, was always 0). Survives BVH reordering via TriGpu memcpy.
- **EmissiveGpu range fix:** `tri_start`/`tri_end` on the GPU path were set from
  pre-BVH `tri_offset` but used after BVH reordering scattered triangles. Fixed by
  scanning the combined array for matching `mesh_idx` after BVH build.

### glTF Extension Support
- **KHR_materials_transmission:** Parsed and applied. Mesh 1 classified as "glass"
  with `transmission=1.0`, `ior=1.6`.
- **KHR_materials_ior:** Parsed and applied. IOR per-material (1.6 for the glass sphere).
- **KHR_materials_volume:** Parsed (`thicknessFactor`) but not used by the shader.
- **KHR_materials_iridescence:** **Not parsed** — the extension object is silently
  skipped in `gltf_parser.cc`. No iridescence fields exist on `GltfMaterial`, and
  the thickness texture (IridescenceLamp image idx 2) is never loaded into any
  mesh. (Earlier docs incorrectly stated this was "parsed".)

### glTF Textures
- **baseColorTexture:** Done — sRGB→linear, bilinear, wrap-repeat, both backends.
- **metallicRoughnessTexture (ORM):** G channel (roughness) × `roughnessFactor`,
  B (metallic) × `metallicFactor`, both backends. B is consumed by the unified
  plastic/metallic PBR (diffuse × (1−metallic), F0 = mix(0.04, basecolor,
  metallic) specular, F0-weighted mirror). R (occlusion/AO) is consumed as a
  per-pixel multiplier on the ambient term and the per-light diffuse terms
  (plastic/metallic, glass, subsurface, and the emissive-surface diffuse pass);
  the specular lobe and the F0-weighted mirror are **not** AO-attenuated
  (Phase 1 item 4 — rationale in the Investigation Log).
- **occlusionTexture:** References the same ORM texture in IridescenceLamp;
  its R channel is the AO consumed above (the `index` key is the same ORM
  image the parser already loads via `pbrMetallicRoughnessTexture`).
- **normalTexture / emissiveTexture:** Not implemented (IridescenceLamp uses neither).

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

Material/texture plan, anchored on **IridescenceLamp**
(`test_scenes/IridescenceLamp/`, scene `test_scenes/scene_lamp.json`). The
model exercises every open item: three materials (metal shade, transmission
glass sphere with iridescence, iridescent body), plus base color + ORM +
iridescence-thickness textures. The other test scenes (Box, Suzanne, Lantern,
...) use none of these extensions.

### Open Bugs
1. **ORM roughness CPU/GPU parity.** CPU `renderer.cc` (ORM roughness
   override block in the mesh-hit shading path) reads ORM bytes
   directly (correct — glTF ORM is linear data). GPU `shaders.metal` (ORM
   roughness override in trace_ray, calls `sample_base_color`) samples
   through that function, which applies an sRGB→linear conversion,
   so GPU roughness values are systematically off.
   **Fix:** add a linear (non-sRGB) sampler for the ORM texture on the GPU side.

### Phase 1 — Per-pixel PBR foundation
Goal: plastic/metallic become one PBR material parameterized per pixel, which
is how glTF actually expresses it. IridescenceLamp's materials have no explicit
`metallicFactor`, so the spec default (1.0) currently classifies the shade and
body as hard `MAT_METALLIC` mirrors (`gltf_parser.cc`, metallic/plastic
class-split branch, `else if (metallic > 0.5f)`) — the ORM B channel
carrying the real per-pixel metalness is never consulted.

1. Fix the ORM parity bug above.
2. Add per-pixel `metallic = ORM.B × metallicFactor` and `ao = ORM.R` —
   new fields on CPU `MeshObjData` (`include/types.h`) and GPU `MeshMat`
   (`shaders.metal` + upload in `gpu_renderer.mm`).
3. Merge `MAT_PLASTIC` / `MAT_METALLIC` in both shading paths:
    diffuse `basecolor × (1−metallic) × N·L`,
    specular `F0 = mix(0.04, basecolor, metallic)`.
    `MAT_GLASS` and `MAT_EMISSIVE` remain separate classes.
4. DONE (item 4): Multiply AO into the ambient and per-light diffuse terms in
   both backends; specular lobe + F0 mirror deliberately untouched.
5. DONE (item 5): Rebaseline after item 4 — 127,575 differing pixels
   (Investigation Log, "CPU/GPU AE Baseline").

### Phase 2 — KHR_materials_iridescence
The visual payoff of the model; the extension is not even parsed today.

1. **Parse:** extend the `GltfMaterial` struct (`gltf_parser.cc`) with
   `iridescenceFactor`, `iridescenceIor`, `iridescenceThicknessMin/Max`,
   `iridescenceThicknessTex`; read the extension in `parse_materials`.
   IridescenceLamp values: sphere ior 2.0 / 385–405 nm, body ior 1.8 / 485–515 nm,
   both with thickness texture idx 2.
2. **Load:** thickness texture (image idx 2, **linear data**) into the texture
   array; new `iri_tex_index` per mesh, mirroring the `orm_tex_index` plumbing
   (`MeshObj` → `MeshObjData` → `MeshMat`).
3. **Shader (both backends):** thin-film interference tint on the
   specular/reflection lobe. 3-wavelength analytic thin-film model (~2
   internal bounces), thickness = `min + (max−min) × texel`, `cos θ₂` via
   Snell's law at the extension IOR; add the tint weighted by
   `iridescenceFactor` (angle blend per the KHR reference shader).

### Phase 3 — KHR_materials_volume absorption
`thicknessFactor` (0.005 on the lamp sphere) → Beer–Lambert absorption in the
transmission path. Low visual impact on this model — do after Phase 2.

### Medium Term
- **Punctual lights (KHR_lights_punctual)** from glTF.
- **Normal mapping** — not needed for IridescenceLamp (no `normalTexture` in
  the file); pursue with a model that uses one.
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

### CPU/GPU AE Baseline — scene_lamp.json (768×1024)

**Status:** BASELINE ESTABLISHED — current: **127,575** differing px /
sum_abs_err 10,676,607 / max channel err 80 (masked: inside 110,398 /
10,619,930; outside 17,177 / 56,677) — post-item-4 AO, commit 91bae3b.
**Method:** `make`; render CPU (`./ray2 --cpu <scene>`) and GPU (`./ray2 <scene>`)
with a copy of `test_scenes/scene_lamp.json` that omits the `"output"` key, so each
writes its PPM to stdout (a `28T`/`GPU` prefix line precedes the PPM header — the
diff tool scans for `P6\n`). Diff with `tools/ppm_diff.py`: counts pixels where any
8-bit channel differs, and reports sum_abs_err (sum of per-channel |diff|) and max
channel error. Both backends are deterministic (re-renders are byte-identical), so
the count is exact, not a noise level.

**Measured:** at 183750c — 132,978 differing pixels (16.91%), sum_abs_err
12,274,209. Identical at 96266de. At 9fb7c92 (pre-ORM-parity-fix) the AE was
267,361 (34.00%), sum_abs_err 13,096,122 — the ORM fix (96266de) halved it.

**Rebaseline after the item-3 merge** (unified `MAT_PLASTIC`/`MAT_METALLIC`
PBR: diffuse × (1−metallic), F0 = mix(0.04, basecolor, metallic) specular,
and the mirror reflection restored as an F0-weighted term of the unified
material — commit recording this baseline) — **127,611** differing pixels
(16.23%), sum_abs_err 12,260,697, max channel err 76. Masked split: inside
110,416 / 12,205,351; outside 17,195 / 55,346. The 5,367-pixel drop vs
132,978 is real parity from the unified material (see the glass-region
section: the outside pixels are all lamp metal, now F0-tinted per pixel);
the residual is the same documented CPU-recursive-vs-GPU-iterative split,
not noise to tune toward. Verified reproducible at the item-4 baseline
(HEAD before the AO change): identical three-way split. Superseded as the
current baseline by the item-4 AO rebaseline below.

**Rebaseline after item 4 (AO) — CURRENT BASELINE** — AO (ORM.R) multiplied
into the ambient term and the per-light diffuse terms in both backends
(unified PBR, glass, subsurface, and the emissive-surface diffuse pass); the
specular lobe and the F0-weighted mirror are deliberately NOT AO-attenuated
(decision + rationale below). Same canonical mask — **127,575** differing
pixels (16.22%), sum_abs_err 10,676,607, max channel err 80. Masked split:
inside 110,398 / 10,619,930; outside 17,177 / 56,677. This is the current
reference for future CPU/GPU parity work on this scene. Tripwire check: the
differing-pixel
*count* moved by only −36 (−0.03%) and the spatial split is unchanged
(86.53% → 86.54% inside share) — the same pixels, so CPU/GPU agreement
holds at the established tolerance and nothing was patched. The AE
*magnitude* (sum_abs_err) fell ~13% overall: the glass-path residual error
scales with local brightness, and AO darkens the occluded glass region, so
the same traversals now differ by smaller 8-bit deltas. Lower is not better
here; the counts are the parity signal.

**Item-4 decision — AO does NOT attenuate specular or the mirror.**
Baked AO (the ORM.R channel) is a hemispherical-occlusion estimate: it
models how much of the local hemisphere is blocked, which is exactly what
attenuates ambient/indirect light and (approximately) direct *diffuse*
light, whose BRDF integrates over that same hemisphere. The specular lobe
responds from a single narrow direction (the half-vector direction), and
the F0-weighted mirror is a direct directional reflection of scene
geometry — hemispherical AO carries no information about occlusion along
those specific directions. Straight-multiplying specular by diffuse AO
produces the classic "highlights dim in crevices" dusty-metal artifact;
renderers that DO apply AO to specular use contact-hardening *boosts*
(local curvature raising F0 in crevices), not plain attenuation (three.js
`aoMap`, the target framework for this model, applies occlusion to
indirect diffuse only). Keeping specular/mirror untouched also keeps the
item-3 F0 work intact and makes the pixel delta attributable to exactly
the shaded ambient/diffuse pixels.

**Item-4 judgment calls:** (1) AO is applied to `MAT_GLASS`'s ambient and
per-light diffuse — an explicit choice, not an artifact of reading
"ambient + per-light diffuse" as material-agnostic. The glass material
(IridescenceLampTransmissionIridescence) references the same ORM atlas, so
valid baked AO exists in its UV region and item 2's plumbing already samples
`sao` per pixel on the glass path; the glass branch has the same shared
ambient term and a plain `sc × diff × lf` diffuse as every other material, so
consuming AO there is consistent. The exposure is small: AO touches only the
glass surface's ambient/diffuse film — its dominant contributions (transmitted
light scaled by base color, IOR/F0 reflections) are the specular/directional
terms the spec decision already excludes. Exempting glass would special-case
a single material on the shared ambient line for no physical reason; item 3's
"MAT_GLASS stays separate and untouched" meant not merging it into the unified
PBR class, not shielding it from AO. (The subsurface branch follows the same
uniform rule — vacuous here: no subsurface meshes in this scene and analytic
spheres carry `ao = 1.0`; emissive returns before these terms so is
structurally unaffected; the floor has no UVs/ORM so is unchanged.)
(2) The emissive-surface light path's diffuse pass
(`sc × kd × G/pdf`) also takes AO — it is a per-light diffuse in
scenes that use emissive lights; IridescenceLamp has no emissive
material (num_emissive = 0) so that path is untested by this render.
(3) In each backend `× ao` was inserted at one fixed position in each
product, preserving every pre-existing factor order (no reassociation).

**Origin check for 133,752:** a previously cited figure of 133,752 (17.0%) does not
appear anywhere in repo history — `git log -S "133,752" -S "133752" --all` returns
no commits, and no doc ever recorded it (the companion glass-path figure, 108,003,
is likewise absent). It is not reproducible in any tested state (9fb7c92 / 96266de /
183750c are all byte-deterministic). Treat **132,978** as the pre-item-3
baseline (superseded — see "Rebaseline after the item-3 merge" above).

### Glass-Region Share of the CPU/GPU AE — scene_lamp.json (768×1024)

**Status:** MEASURED
**Goal:** attribute the 132,978-pixel AE from the baseline above to the glass
sphere (mesh 1, lamp_transmission). A figure of 108,003 was previously cited as
the "glass-path component" with no recorded provenance; it is **not
reproducible** under the method below.

**Method (rendered mask; renderer exposes color output only, so no object-ID
mask exists and none was added):** build "the same scene minus the glass" by
pointing `test_scenes/scene_lamp_no_glass.json` (identical camera/lights/env as
`scene_lamp.json`, no floor, no `output` key so PPM goes to stdout) at
`test_scenes/IridescenceLamp/IridescenceLamp_no_sphere.gltf` (the full glTF with
lamp_transmission's `primitives` emptied; all other buffers/materials
byte-identical). Mask = pixels whose rendered image changes when the glass is
removed, ORed across both backends:

    ppm_diff.py make-mask mask.ppm full_cpu.ppm noglass_cpu.ppm full_gpu.ppm noglass_gpu.ppm

The CPU/GPU glass-traversal divergence can only surface in such pixels, so this
is the correct region for spatial attribution. Residuals: the region also
includes a few shadowed body pixels (the sphere occludes the lights), and it
excludes only AE pixels whose glass contribution is below 1/255 (none
measurable). Both backends and both scenes are byte-deterministic, so the whole
pipeline is reproducible.

**Region:** 147,504 px (18.76% of frame); bbox x=[51,767] y=[517,1023]
(sphere clips the bottom-right edge of the frame). Render with
`./ray2 --cpu test_scenes/scene_lamp_no_glass.json` / `./ray2 test_scenes/...`
and the full scene (`scene_lamp.json` without its `output` key), 768×1024.

**Mask file (canonical):** `test_scenes/lamp_glass_mask.ppm` is the masked
region above, committed as a file (force-added: `*.ppm` is gitignored). It was
built from b2c242f renders of the four images per the method, which
reproduces the 147,504-px region exactly, so it is the reference for the
masked AE splits recorded here. Rendered masks are state-dependent — a mask
regenerated at HEAD instead measures 138,975 px (the item-3 F0 re-shade moved
which sphere-occluded body pixels fall below the 1/255 rounding threshold) —
so use the committed file; do not regenerate unless a state change genuinely
moved the glass region and the region count is understood and noted.

**Measured (mask, `ppm_diff.py full_cpu full_gpu mask.ppm`):** — at b2c242f
- inside: 111,785 differing / sum_abs_err 12,214,931 (75.78% of region)
- outside: 21,193 differing / sum_abs_err 59,278 (3.32% of its 638,928 px)
- inside share of total differing: **84.06%**
- mean error 109.3/px inside vs 2.8/px outside — clean separation between the
  glass-path divergence and background float noise.

**Measured after the item-3 merge** (unified plastic/metallic PBR +
F0-weighted mirror; same mask from b2c242f, same method):
- inside: 110,416 differing / sum_abs_err 12,205,351 (74.86% of region)
- outside: 17,195 differing / sum_abs_err 55,346 (2.69% of its 638,928 px)
- inside share of total differing: **86.53%**

**Mask used:** the committed canonical `test_scenes/lamp_glass_mask.ppm`
(see the "Mask file (canonical)" note above the first measurement) — rebuilt
from b2c242f renders for item 4 and verified to reproduce the 147,504-px
region exactly. Split below uses that file, not a HEAD-regenerated mask.

**Measured after item 4 (AO on ambient + per-light diffuse; specular and
F0-weighted mirror untouched; same mask, same method) — CURRENT:**
- inside: 110,398 differing / sum_abs_err 10,619,930 (74.84% of region)
- outside: 17,177 differing / sum_abs_err 56,677 (2.69% of its 638,928 px)
- inside share of total differing: **86.54%**
- pixel count held to −36 (127,611 → 127,575); the AE *magnitude* fell
  ~13% (12,260,697 → 10,676,607) because the glass-path residual divergence
  is scaled down by the common AO factor in occluded regions — same pixels,
  smaller errors, not a parity fix.

**Current glass-region share:** treat **110,398** (rendered mask, post-item-4
state) as the current glass-region share of the AE, superseding 110,416
(post-item-3) and 111,785 (pre-item-3). As before it is a spatial count of
differing pixels in the glass-influenced region, not a causal
traversal-only count.

Composition of the outside band (diagnosed at b2c242f with a sky-only
reference render): **all** 21,193 outside differing pixels sat on the lamp
shade/body (the `MAT_METALLIC` meshes); zero on sky — sky
pixels are bit-identical across backends. The item-3 merge re-shades exactly
those metal pixels (F0-tinted mirror per pixel, no diffuse at metallic=1,
spec gain 0.4 vs 0.8), so the outside count moves with them (21,193 →
17,195); that is expected and not a parity improvement to chase. A near-black
metal reading with an outside count around 7k signals the mirror was dropped,
not that the backends agree.

**Cross-check (bbox region `51,517,717,507` = 363,519 px):** inside 114,692
differing / 12,254,088 (86.25% share); the rect overcovers (sky in its
corners), so the rendered mask is the preferred definition.

**108,003 verdict:** not reproducible — the mask gives 111,785 and the bbox
114,692; no committed run ever recorded 108,003. After the item-3 merge the mask
measures 110,416 (below). Treat 111,785 (rendered mask) as the
pre-item-3 glass-region share. It is a spatial count of differing pixels in the
glass-influenced region, not a causal traversal-only count — that split is not
separable from float noise out of two images.

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
- **Extensions:** KHR_materials_transmission, KHR_materials_ior parsed and
  applied. KHR_materials_volume parsed, unused. KHR_materials_iridescence not
   parsed. Textures: baseColorTexture + ORM wired on both backends — G
   (roughness), B (metallic, in the unified plastic/metallic PBR), R
   (AO on ambient + per-light diffuse; specular/F0-mirror un-AO'd, item 4).
- **Diagnostics:** `--mesh-stats` flag, per-mesh degenerate triangle analysis, accessor
  metadata, index range checks, JSON output to `mesh_stats.json`.
- **Tested with:** Box, Suzanne, Lantern, WaterBottle, Avocado, BoomBox,
  MetalRoughSpheres, IridescenceLamp (primary target).
