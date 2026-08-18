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
- **KHR_materials_volume:** **Fully supported** (Phase 3 complete: parse →
  plumbing → Beer–Lambert absorption on both backends, ray-traced path).
  `GltfMaterial` carries `vol_th`, `att_r/g/b`, `att_dist`, `vol_tex`;
  the shader charges `T = exp(−σ·x)` per in-medium segment on the
  downstream reflection/refraction (three.js `volumeAttenuation` verbatim,
  `include/volume.h` + MSL mirror; `make volcheck` parity 8.965e-08).
  `thicknessTexture` is parsed/reported but not sampled.
- **KHR_materials_iridescence:** **Fully supported** (Phase 2 complete:
  parse → load/plumbing → thin-film shader on both backends).
  `GltfMaterial` carries `iri_factor`, `iri_ior`, `iri_thin_min/max` (nm)
  and the `iridescenceThicknessTexture` index; all five flow into every mesh
  (`MeshObj` → CPU `MeshObjData` / GPU 80-byte `MeshMat` + linear
  `texture(3)` upload). Both backends evaluate the verbatim three.js
  thin-film model (`include/thin_film.h`, shared with the MSL mirror in
  `shaders.metal`) and blend it into the per-surface F0. Thickness reads
  the texture's **G channel** (parity with three.js's `.g` read — the
  texture's data lives in G; the KHR spec says red). `--mesh-stats`
  reports the values via `[gltf:mat]`/`[mat]` lines.

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

### Phase 1 — Per-pixel PBR foundation
**All five items done.** Goal: plastic/metallic as one PBR material
parameterized per pixel — how glTF actually expresses it. (Original
motivation: IridescenceLamp's materials have no explicit `metallicFactor`, so
the spec default (1.0) classified the shade and body as hard `MAT_METALLIC`
mirrors — `gltf_parser.cc`, class-split branch `else if (metallic > 0.5f)` —
and the ORM B channel carrying the real per-pixel metalness was never
consulted. The classification label is retained at import; per-pixel shading
is now driven by the ORM B channel in both backends.)

1. DONE (item 1): **ORM roughness CPU/GPU parity fixed** (commit 96266de) —
   the GPU samples `metallicRoughnessTexture` through a linear (non-sRGB)
   sampler instead of the sRGB→linear base-color path; A/B-verified in
   "CPU/GPU AE Baseline" below (267,361 → 132,978 px, halved).
2. DONE (item 2): per-pixel `metallic = ORM.B × metallicFactor` and
   `ao = ORM.R` added as fields on CPU `MeshObjData` (`include/types.h`) and
   GPU `MeshMat` (`shaders.metal` + upload in `gpu_renderer.mm`;
   commit 183750c).
3. DONE (item 3): `MAT_PLASTIC` / `MAT_METALLIC` merged in both shading paths:
   diffuse `basecolor × (1−metallic) × N·L`,
   specular `F0 = mix(0.04, basecolor, metallic)`.
   `MAT_GLASS` and `MAT_EMISSIVE` remain separate classes (commit 028e9c4;
   rebaseline in "Rebaseline after the item-3 merge" below).
4. DONE (item 4): Multiply AO into the ambient and per-light diffuse terms in
   both backends; specular lobe + F0 mirror deliberately untouched.
5. DONE (item 5): Rebaseline after item 4 — 127,575 differing pixels
   (Investigation Log, "CPU/GPU AE Baseline").

### Phase 2 — KHR_materials_iridescence
The visual payoff of the model. **All three items done** (parse,
plumbing, shader on both backends).

1. DONE (item 1): **Parse.** `GltfMaterial` (`gltf_parser.cc`) gained
   `iri_factor`, `iri_ior`, `iri_thin_min/max` (nm) and `iri_tex`
   (the `iridescenceThicknessTexture` index); a `KHR_materials_iridescence`
   branch reads them in `parse_materials`. Spec defaults applied
   (factor 0, ior 1.3, 100–400 nm, no texture). Verified via a `--mesh-stats`
   `[gltf:mat]` line on all three IridescenceLamp glTF variants — exact values
   match the file: material 1 (glass sphere) iri 1.0 / ior 2.0 / 385–405 nm /
    tex idx 2; material 2 (body) iri 1.0 / ior 1.8 / 485–515 nm / tex idx 2;
   material 0 steady on spec defaults. Parse-only change: CPU/GPU renders are
   byte-identical pre/post and the CPU/GPU AE baseline is unchanged
   (127,575 px / 110,398 inside / 17,177 outside).
2. DONE (item 2): **Load/plumbing.** Note: the thickness texture (image idx 2)
   was *already* decoded into the texture array — `parse_images` decodes every
   image in the file — so no loader change; only plumbing. All five values now
   flow `GltfMaterial` → `MeshObj` → (`MeshObjData` CPU @renderer.cc,
   `MeshMatGpu`→`MeshMat` GPU). GPU: `MeshMat` extended to 80 bytes
   (`static_assert` updated), thickness texture uploaded as `MTLPixelFormat-
   RGBA8Unorm` (linear — `sample_linear`, no sRGB) at `texture(3)`, bound
   single-slate per the existing base-color/ORM upload pattern (the per-mesh
   `iri_tex_index` is a validity flag, not a selector — all meshes in this
   model share the texture). Verified: per-mesh `[mat]` lines exact on all
   three glTF variants (incl. `iri_tex=2` mapping); OBJ `parse_mesh` path
   initializes all five fields (explicit, since that array is `realloc`'d, not
   zeroed); CPU+GPU renders byte-identical pre/post (GPU identity proves the
   80-byte Metal layout match); AE unchanged at 127,575 / 110,398 / 17,177.
3. DONE (item 3): **Shader (both backends).** The plan above was
   deliberately superseded: instead of a custom 3-wavelength fit we ported
   **verbatim the exact model the web viewer uses** — the fused
   three.js/Belcour analytic thin-film (DC term + 2 interference orders
   vs CIE-XYZ spectral sensitivities in Fourier space). Sources (vendored):
   `evalIridescence` from
   `web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
   iridescence_fragment.glsl.js`, `F_Schlick` (Epic exp2 variant) from
   `common.glsl.js`, `Schlick_to_F0` from
   `lights_physical_pars_fragment.glsl.js`. Single C implementation in
   `include/thin_film.h` (static inline); `shaders.metal` mirrors it in
   MSL with identical constants. Blend:
   `F0_eff = mix(baseF0, Schlick_to_F0(film, 1.0, cosV), iridescenceFactor)`
   where cosV = clamp(|N·V|, 0, 1) is the *view* angle, outsideIOR = 1.0,
   and the substrate IOR is derived from the surface F0. Glass is
   special-cased: film applied on entering hits only, per-channel
   reflection/transmission weights (energy-complementary
   `max(0.0, 1.0 − wr)`), glass baseF0 = r0·sc with
   r0 = ((ior−1)/(ior+1))². Thickness = green channel of the
   `iridescenceThicknessTexture` (`d = min + (max − min) × G`), exactly the
   three.js read (`lights_physical_fragment.glsl.js:97`):
    `material.iridescenceThickness = (iridescenceThicknessMaximum -
    iridescenceThicknessMinimum) * texture2D( iridescenceThicknessMap,
    vIridescenceThicknessMapUv ).g + iridescenceThicknessMinimum;` The
    KHR spec says the channel is red, but this texture’s data lives in G
    (R=B=0, G 0–247) — deliberate
    deviation from the spec letter for viewer parity, documented in the file. Film is clamped ≥ 0;
   `Schlick_to_F0` output may go slightly negative (36 of 216 grid rows);
   the weights clamp it — same as GLSL falling through with no clamp.
   **Parity:** `tools/iri_check.c` (float32, 6 cosV × 6 d × 2 ior × 3
   baseF0 grid) vs `tools/iri_ref_check.mjs` (float64 port of the exact
   chunks, with a constant self-check that emits the glsl function text
   and diffs the numeric literals) — max abs diff **1.5e-6** (gate 1e-3),
   PASS. **Control gate:** Box CPU render byte-identical pre/post
   (`iri_factor > 0` gate ⇒ zero pixels change for scenes without the
   extension). **Visual:** step-2-vs-step-3 CPU pixel diff localizes
   changes to exactly the iridescent surfaces — glass sphere (green band
   at grazing, magenta near normal) + iridescent base ring; plain
   metal shade / sky / floor have *zero* changed pixels. Model
   numerics agree with the observed hues (verify: parity grid, glass-like
   baseF0=0.04 → film (0.23, 0.36, 0.19) green at cosV=0.30 and
   (0.27, 0.09, 0.16) magenta at cosV≥0.75; white-metal baseF0 ≈ (1,1,1)
   stays neutral). GPU render compiles clean and shows the same tint.
   **Rebaseline (new current, see Investigation Log):** 127,582 (16.22%)
   differing px / sum_abs_err 9,682,442 / max 80; inside 110,453
   (74.88%) / outside 17,129 (2.68%); inside share 86.57% — count
   stable vs the item-4 baseline (Δ+7 px): same jittering pixels,
   no new divergence class introduced by the shader.

### Phase 3 — KHR_materials_volume absorption
`thicknessFactor` (0.005 on the lamp sphere) → Beer–Lambert absorption.
**All four items done** (parse/plumbing, model + parity harness, medium state
machine on both backends, physical refraction for the volume faces).

1. DONE (item 1): **Parse + plumbing.** `GltfMaterial`
   (`gltf_parser.cc`) gained `vol_th` (thicknessFactor), `att_r/g/b`,
   `att_dist`, `vol_tex` (the `thicknessTexture` index); spec defaults
   applied (0 / [1,1,1] / +inf / none); a `KHR_materials_volume` branch
   reads them in `parse_materials`. `--mesh-stats` reports them: the
   `[gltf:mat]` and `[mat]` lines gained `vol_th= att=(...) att_d=`
   (verified exact: lamp mat 1 `vol_th=0.005 att=(1,1,1) att_d=inf`;
   mats 0/2 on defaults). Values flow `GltfMaterial` → `MeshObj` →
   (CPU `MeshObjData` @renderer.cc, GPU `MeshMatGpu`→`MeshMat`);
   `MeshMat` extended 80→104 bytes (`static_assert` updated, upload
   copies in `gpu_renderer.mm`); the scene-JSON `parse_mesh` path
   initializes the fields explicitly (same realloc-not-zeroed trap as
   Phase 2 item 2). Parse/plumbing-only gate: CPU+GPU renders of every
   existing scene byte-identical pre/post (defaults ⇒ T ≡ 1 exactly).
2. DONE (item 2): **Model + parity harness.** Deliberately ported
   **verbatim the exact model the web viewer uses** — three.js
   `volumeAttenuation` from
   `web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
   transmission_pars_fragment.glsl.js`: `σ = −log(attenuationColor) /
   attenuationDistance` per channel, `T = exp(−σ·x)` with the
   inf-distance short-circuit. Single C implementation in
   `include/volume.h` (`vol_transmittance`, `vol_sigma_nonzero`);
   `shaders.metal` mirrors it in MSL with identical constants (same
   convention as `thin_film.h`). **Parity:** `tools/vol_check.c`
   (float32, 168-point grid over distance/color/distance-infinity
   corners) vs `tools/vol_ref_check.mjs` (float64 port of the exact
   GLSL text, with the usual literal self-check) via
   `tools/vol_diff.py` — `make volcheck`: max abs diff **8.965e-08**
   (gate 1e-3), PASS.
3. DONE (item 3): **Medium state machine (both backends).** A ray
   carries the medium it is traveling in: CPU `Medium {ior, cr, cg, cb,
   att_dist}` (`med_air()` = default) passed through the recursive
   `trace_ray`; GPU `stk_md[]` (float4: ior, cr, cg, cb) + `stk_ma[]`
   (attenuation distance) on the iterative stack, air/`INFINITY`
   pre-filled. Charge: at any hit while in an absorbing medium
   (`ior > 1`), `Tseg = vol_transmittance(t_hit, …)` multiplies the
   *downstream* contributions — the reflection ray and the refraction
   ray — i.e. each segment inside the medium is charged exactly once, on
   both paths. Entry/exit classification: hit normals are **always
   flipped to face the ray** (`hit_mesh_bvh` / the GPU mirror), so the
   legacy `entering = cos_i < 0` flag can never fire on a mesh face;
   `hit_mesh_bvh` therefore also reports the **pre-flip** sign
   (`side_out`: front / backface of the stored normal — a shell's
   stored normal always faces the non-medium side, so front-cross =
   enter the volume, back-cross = exit to air; verified correct for the
   two-surface lamp wall, item 4 note). A `thicknessFactor > 0`
   boundary sets/clears the medium accordingly; otherwise the incoming
   medium carries (homogeneous). Two fixes found while building item 3:
   (a) **environment escape:** a ray leaving the scene while inside an
   absorbing medium returns 0 (T(∞) = 0) instead of the raw environment
   — previously dead code, now the legitimate end state; (b) **seam
   push:** an origin offset along the refracted direction leaves the
   ray glued to the entry seam, where a neighbor triangle's backface ~1e-4
   later produced a spurious in-medium hit that falsely "exited" the
   medium before the real chord was charged; on volume entry the
   refracted origin is additionally pushed along the (flipped) normal
   (`+ n·EPS`), straight into the volume. **Control gate** (gated on
   `vol_sigma_nonzero` — σ ≠ 0): renders of every scene without real
   attenuation are **byte-identical to the HEAD build**, 0 differing
   pixels, both backends.
4. DONE (item 4): **Physical refraction for the absorbing-volume faces.**
   Found while verifying item 3: under the always-flipped-normal
   convention, `entering` never fires on mesh glass, so *every* legacy
   glass hit was shaded with the air→air "exit" parameters — the entry
   inverts its refraction bending and TIR-rings above
   sin θᵢ ≈ 1/ior, and the *exit* refracts the ray back into the glass,
   which is where the famous in-glass walk (the 28.7M-hit stat) comes
   from. For an absorbing volume's faces the refraction now uses the
   correct side: front (entering, `side = 1`): outward normal,
   η = 1/ior; back (exiting): in-wall normal (the flipped one),
   η = ior. Everything else — spheres, and any non-volume (or
   non-absorbing, σ = 0) mesh glass — keeps the legacy parameters
   **exactly**, which is what keeps the canonical baseline untouched.
   Consequence for the absorption scene: the in-glass walk is gone
   (clean front→wall→hollow→wall→back traversal, two real chords per
   pass) and the intra-back-end parity actually *improves* there
   (tripwire below).

**Control / tripwires (post Phase 3, current):**
- **Control gate:** new build vs the git-HEAD baseline build
  (`/tmp/ray_base`, `git archive HEAD | tar -x`): lamp scene CPU **0**
  differing px / sum_abs_err 0 / max 0, GPU **0** / 0 / 0 — byte-
  identical. (The σ ≠ 0 gate; IridescenceLamp's own sphere carries
  `thicknessFactor = 0.005` but white/+inf attenuation.)
- **Canonical lamp AE (masked, same canonical mask):** **127,582**
  (16.22%) / 9,682,442 / max 80; inside 110,453 (74.88%) / outside
  17,129 (2.68%); inside share 86.57% — **unchanged** vs the Phase 2
  current baseline above, exactly as the gate requires.
- **New absorption reference (new scene, for the record):**
  `test_scenes/IridescenceLamp/IridescenceLamp_absorption.gltf`
  (lamp mat 1 volume: `thicknessFactor 0.005`,
  `attenuationColor [0.7, 0.45, 0.25]`, `attenuationDistance 0.2`;
  shares the external `IridescenceLamp.bin`, no copy) rendered by
  `test_scenes/scene_lamp_absorption.json` (output
  `images/test_lamp_absorption.png`) / `..._stdout.json` (PPM stdout):
  CPU/GPU AE with the canonical mask — **126,852** (16.13%) /
  7,619,587 / max 85; inside 109,720 (74.38%) / outside 17,132 (2.68%);
  inside share 86.49% — the same documented glass-traversal class as
  the canonical scene (slightly *better*: the corrected refraction
  aligns the two backends' in-glass paths). Zone check (absorption vs
  the lamp render): all **108,953** changed pixels sit in the sphere
  zone (y ≥ 517), **0** outside; **0** bright→near-black pixels.
  Mean zone delta R −5.4 / G −8.2 / B −6.2 (max 101) at d = 0.2; a
  strong variant (d = 0.08, temp asset, not committed) gives R −61.9 /
  G −55.7 / B −35.5 (max 176, n = 192,915) — strictly monotone in
  1/distance. The per-channel *ordering* of a zonewise mean is not the
  Beer–Lambert order because the delta mixes the two T-sites (the
  transmitted part, blue-dominant as expected, plus the in-medium
  re-reflection part, white-dominant base) and the item-4 refraction
  re-lighting — `T` itself is verified per-term by the volcheck grid.
  Visual: the lamp interior (iridescent body) reads clearly through the
  glass with a subtle warm cast; sky-through-glass is not blackened;
  floor/shade outside the sphere pixel-identical to the lamp render.

**Phase 3 judgment calls:**
1. **Actual ray-traced distance** (planned Option B, user-approved) per
   the KHR instruction for ray-traced renderers, instead of the
   thicknessFactor proxy three.js's *shading* model uses —
   `vol_transmittance`'s `x` argument is the measured in-medium path
   length, segment by segment. Consequence, confirmed against the data:
   IridescenceLamp's glass is a **two-surface thin wall** — 2,990
   verts = 2 × 1,495, radii span 0.0939→0.0980 (wall ≈ 0.0041, matching
   `thicknessFactor` 0.005), measured from the `.bin` position
   accessor — so the true in-glass path per pass is ≈ 2·wall/cosθ
   (≈0.008–0.02), not a solid-sphere diameter. That is exactly why the
   absorption is a subtle warm cast rather than a tinted filter on
   this model (the original plan's "low visual impact" prediction
   held), while a solid glass sphere with the same attenuation params
   would be strongly tinted.
2. **`thicknessTexture`: parse + report only** (user-approved) — the
   `vol_tex` index is stored and shown by `--mesh-stats`; no per-pixel
   sampling (a path-length texture needs the ray, not a surface UV;
   out of scope for this phase).
3. **Surface terms at a hit are not attenuated** (base color,
   specular lobe, shadow rays, emissive) — only light that has *traveled*
   through the medium (the downstream reflection/refraction
   contributions) is charged. Consistent single-slot medium: nested
   volumes are not supported (pre-existing limitation, same as the
   single `skip` mesh); two different media on one path are impossible
   in this scene graph anyway.
4. **Tessellation-seam stubs** (documented, not chased): at the entry
   seam a neighbor triangle's backface ~1.4e-3 behind the hit flips the
   medium to air for one segment, re-set at the next boundary; the
   charge there is ~1 (T(0.0014) ≈ 0.99) — a ~1–2% slice per pass,
   not worth the per-mesh region plumbing it would take to eliminate.
5. **Legacy refraction remains on the non-volume glass path**
   (spheres are correct — their normals are not flipped; any mesh glass
   without σ > 0 keeps the inverted-entry/exit bug as-is to preserve
   the canonical baseline). Fixing it generally would move the 127,582
   baseline and deserves its own phase with its own rebaseline.

### Rebaseline note (no rebaseline occurred)
Phase 3 added code but changed **zero** pixels on every existing scene
(the σ ≠ 0 gate — see item 3/4 above); the current CPU/GPU baseline of
record remains the Phase 2 one: **127,582 / 9,682,442 / max 80**,
inside 110,453 / outside 17,129, inside share 86.57%. The absorption
scene above is a new reference for its own new scene, not a rebaseline.

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
- **Legacy glass refraction is wrong on mesh faces (pre-existing):** hit normals are
  always flipped to face the ray, so the `entering` flag never fires on mesh glass and
  every hit is shaded with air→air "exit" parameters — inverted entry bending
  (TIR ring above sin θᵢ ≈ 1/ior), and the exit refracts the ray back into the glass
  (the in-glass walk). Phase 3 corrected this *only* for absorbing-volume faces
  (σ > 0) so the canonical 127,582 baseline stays untouched; general mesh-glass
  refraction still carries the bug and will need its own phase + rebaseline
  (spheres are unaffected — their normals are not pre-flipped).
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

**Status:** BASELINE ESTABLISHED — current: **127,582** differing px /
sum_abs_err 9,682,442 / max channel err 80 (masked: inside 110,453 /
9,626,805; outside 17,129 / 55,637) — post-Phase-2-item-3 iridescence
(shaded renders with the thin-film model active). Superseded baseline:
127,575 / 10,676,607 / max 80 (inside 110,398 / outside 17,177) —
post-item-4 AO, commit 91bae3b.
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

**Rebaseline after Phase 2 item 3 (iridescence shader) — CURRENT
BASELINE** — thin-film model active on the sphere (mat 1) and base ring
(mat 2); same canonical mask, same method. — **127,582** differing
pixels (16.22%), sum_abs_err 9,682,442, max channel err 80. Masked
split: inside 110,453 / 9,626,805 (74.88% of region); outside 17,129 /
55,637 (2.68% of its 638,928 px); inside share 86.57%. Tripwire check:
the differing-pixel *count* moved by only +7 (+0.005%) from 127,575 —
the thin-film terms are implemented near-identically on both backends
(the C/MSL port was parity-verified at 1.5e-6 against the exact
three.js chunks), so no new class of diverging pixels appeared; the same
jittering pixels remain. The AE *magnitude* fell ~9%
(10,676,607 → 9,682,442): replacing the raw F0-derived lobe/mirror
weights with film-weighted ones changes the residual scale of the
same glass-path traversal divergence (magnitude moves like the item-4
AO case; the count is the parity signal). This is the current reference
for future CPU/GPU parity work on this scene.

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

**Measured after Phase 2 item 3 (iridescence shader; same mask, same
method) — CURRENT:**
- inside: 110,453 differing / sum_abs_err 9,626,805 (74.88% of region)
- outside: 17,129 differing / sum_abs_err 55,637 (2.68% of its 638,928 px)
- inside share of total differing: **86.57%**
- count stable vs item-4 (127,575 → 127,582); magnitude fell ~9% as the
  film-weighted lobe/mirror terms rescale the same traversal divergence.

**Current glass-region share:** treat **110,453** (rendered mask,
post-Phase-2-item-3 state) as the current glass-region share of the AE,
superseding 110,398 (post-item-4), 110,416 (post item-3 merge) and
111,785 (pre-item-3). As before it is a spatial count of differing
pixels in the glass-influenced region, not a causal
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
- **Mesh-glass refraction side params (inverted entry / exit reflected back
  in-glass):** FIXED on absorbing-volume faces only (Phase 3 item 4, σ > 0
  gate); the non-volume path still carries the legacy bug to hold the
  canonical baseline — see "Known Limitations"
- **KHR_materials_volume absorption:** DONE (Phase 3, items 1–4; volcheck
  parity 8.965e-08; control gate byte-identical; absorption-scene reference
  recorded in the Phase 3 section)

### glTF Importer
- **Core spec:** Full glTF 2.0 importer (buffers, views, accessors, meshes, nodes, cameras,
  materials, scenes, transforms).
- **Extensions:** KHR_materials_transmission, KHR_materials_ior parsed and
  applied. KHR_materials_volume fully supported (Phase 3: parse + plumbing +
  Beer–Lambert absorption with ray-traced path length on both backends —
  `include/volume.h`, verbatim three.js `volumeAttenuation`; see Phase 3).
  KHR_materials_iridescence
  fully supported (parse + plumbing + thin-film shader on both backends;
  model is a verbatim port of the vendored three.js chunks —
  `include/thin_film.h`; see Phase 2 item 3). Textures: baseColorTexture +
  ORM wired on both backends — G
  (roughness), B (metallic, in the unified plastic/metallic PBR), R
  (AO on ambient + per-light diffuse; specular/F0-mirror un-AO'd, item 4),
  plus the iridescence-thickness texture (G channel, linear, `texture(3)`).
- **Diagnostics:** `--mesh-stats` flag, per-mesh degenerate triangle analysis, accessor
  metadata, index range checks, JSON output to `mesh_stats.json`.
- **Tested with:** Box, Suzanne, Lantern, WaterBottle, Avocado, BoomBox,
  MetalRoughSpheres, IridescenceLamp (primary target).
