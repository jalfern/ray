# IridescentDishWithOlives — delta analysis and gap-closing plan

Scene: `test_scenes/scene_iri_dish.json` →
`test_scenes/IridescentDishWithOlives/IridescentDishWithOlives.gltf`
(Khronos **glTF-Sample-Assets**. First asset that exercises all four supported
extensions at once: `KHR_materials_ior`, `KHR_materials_iridescence`
(factor + color + thickness textures), `KHR_materials_transmission`,
`KHR_materials_volume`. Also first use of `alphaMode: MASK`, a standalone
`occlusionTexture`, and `normalTexture.scale`).

**Reference:** `test_scenes/IridescentDishWithOlives/screenshot_Large.jpg`
(1280×1167, three.js — source of record, per the dragon convention) and
`DassaultPBRSampleRenderer.jpg` (1298×1173, second renderer, slightly lower
angle — useful for disambiguating renderer artifacts from physics).
Side-by-side: `images/iri_dish_sidebyside.png`
(regenerate: `python3 tools/sidebyside.py <ref.png> <render.ppm> out.png`).

The subject: a gold-leaf charger dish of pitted olives under a tilted
iridescent glass dome. Black background, bright studio IBL.

## Setup (what was matched)

- **Geometry** (world bboxes, from the verified node transforms):
  - glassDish: 2-shell thin plate (2048 tris), r 0.266, y∈[0.001, 0.041].
    Vertical-ray probe: mostly 2 crossings; a central region crosses 4
    (the bowl bottom is a separate closed cap). NOT a single shell — unlike
    the dragon, there are no long in-medium chords.
  - olives: 10,992 verts, y up to 0.081, pitted (holes visible in reference).
  - glassCover: 2-shell dome (1857 verts), y up to 0.362, tilted. Node
    `glassCover_animation` carries a 6.033 s rotation animation; the static
    node pose equals the t=0 keyframe and matches the reference silhouette,
    so no animation evaluation is needed for parity.
  - goldLeaf: textured sheet, r 0.265, y∈[0, 0.038], `alphaMode: MASK`.
- **Materials** (`--mesh-stats` verified): dish = transmission 1.0, iri 1.0
  [500,550] nm + iridescenceTexture/thicknessTexture (same image 0),
  roughness 0.07; cover = transmission 1.0, **ior 1.5** (KHR_materials_ior),
  iri 1.0 [500,550] + textures, vol_th 0.1 + thicknessTexture, ORM is a
  constant 1-bit image (R=255 → AO 1.0, G=26 → metallic ≈ 0.10, B=0 →
  roughness 0 = mirror glass); olives = baseColor + ORM + normal map;
  gold = baseColor + ORM + normal map + MASK.
- **Camera**: glTF `Camera001` (node 5, camera index 0): pos (0.8, 0.5, 0),
  quat (−0.162, 0.688, 0.162, 0.688) → target (−0.09494, 0.05381, 0.0) — a
  pure 26.5° pitch, no roll — yfov 26.231°. Decomposed into the scene JSON's
  pos/target/fov (exact).
- **Reference lighting**: studio environment IBL + black background (the
  three.js pattern: `scene.environment` set, background black).

## Fixes landed while setting this up (3 latent bugs, 1 broken asset)

All three were invisible in the existing asset set and are verified fixed.

1. **`gltf_parser.cc:1796` — glTF camera fov read from the wrong slot.**
   `cameras[ni]` used the *node* index instead of `cameras[nodes[ni].camera]`.
   Latent because IridescenceLamp's camera node happens to be index 0; here
   the camera is node 5 / camera 0, so yfov read a zeroed slot → fov 0 →
   degenerate framing (all rays parallel). Fixed:
   `cameras[nodes[ni].camera].yfov`. Control: IridescenceLamp parse output
   unchanged (its node index == camera index).
2. **`envmap.cc` — blank line in HDR headers ended the header scan.**
   `hdr_read_line` returned 0 for both EOF and blank lines, and the header
   loop treated 0 as stop. Poly Haven headers put a blank line between the
   `FORMAT` line and the `-Y h +X w` line, so **every real 1k HDR silently
   failed to load**. CPU fell back to `envmap_sample_procedural`, GPU to a
   1×1 dummy (`has_env=0`) — i.e. all prior "environment" work in the repo
   (incl. the dragon's "intensity 7.0" tuning) actually ran against the
   procedural sky + point lights. Fixed: `hdr_read_line` now returns -1 at
   EOF, ≥0 (0 = blank) otherwise; the loop skips blanks.
3. **`gpu_renderer.mm` — env texture upload violated the row-pitch API.**
   RGB data (w·12 bytes/row) uploaded into an RGBA32Float texture
   (w·16 bytes/row) with `bytesPerRow = w·12` — Metal reads that as garbage;
   the sampled env was sheared/incorrect even when the load succeeded.
   Fixed by repacking RGB→RGBA on upload.
   Measured on `test_scenes/scene_envtest*.json` (800×600, sky-visible,
   studio HDR): CPU/GPU diff **288,870 px (60.18%) / max 253 → 278 px
   (0.06%) / max 2** (residual = Metal linear vs manual bilinear + AA).
    [NOTE — the envtest scene carries no material textures (only the
    separately-bound env texture), so it does NOT index the 64-slot bundle
    and was NOT masked by the Metal page fault. The "278 px (0.06%) / max 2"
    residual is a real CPU-vs-GPU reading, not a CPU-vs-CPU false positive.
    It is superseded: the honest cross-backend number at HEAD is 4.08% /
    max 255 (the env-sampling divergence grew when IBL landed). The 60.18%
    pre-upload-fix figure is a real cross-backend divergence (the row-pitch
    bug) and stands.]
4. **Asset breakage: `envmaps/polyhaven_haven_01_1k.hdr` is an HTML page**
   (a botched download), not an HDR. It still fails to load after the fix,
   so the dragon/lamp baselines are **unchanged** by items 1–3 (same
   procedural fallback as before). Re-downloading a real haven_01 (or
   switching the dragon to a real studio HDR) would change those baselines —
   do it as a separate, deliberate task. Added
   `envmaps/polyhaven_studio_small_03_1k.hdr` (valid Radiance RLE, 1024×512,
   load-verified: avg 1.88).

## Phase 1 landed (2026-08-26): opt-in floor + background color

Both items implemented on both backends; baseline binary for the gate was
snapshotted at `/tmp/ray_pre_phase1` (working tree at this writing,
pre-Phase-1).

**1.1 Floor gating.** CPU: `SceneOpts {int has_floor; int has_bg_color;
V bg_color;}` (renderer.cc) passes by value through the recursive
`trace_ray` after `Medium med`; `hit_floor` is now
`opts.has_floor && hit_floor(...)`; `RenderContext` carries the opts.
GPU: `SceneGpu` grew five 4-byte fields (`has_floor`, `has_bg_color`,
`bg_r/g/b`) → **52 → 72 bytes**, appended after `num_textures` on both
sides; `hf0 = has_floor != 0 && hit_floor(...)`; kernel forwards the
fields. Size parity verified *independently on both sides*, not assumed:
host `static_assert(sizeof(SceneGpu) == 72)` (compile-time), an MSL
`static_assert(sizeof(SceneGpu) == 72)` accepted by the runtime shader
compiler, plus a red-background probe render confirming field offsets
end-to-end. The denoiser gbuffer (`denoiser.cc:146`) already gated on
`scene->has_floor` — untouched.
**Gotcha: the Makefile has no header dependency tracking** — adding
fields to `Scene`/`SceneGpu` requires `make clean && make` (a stale
`main.o` reading the old layout wrote a garbage-named output file).
Scene files: `"floor": true` added to all 10 implicit-floor scenes
(dragon ×3, lamp ×6, self_shadow); gate vs baseline **byte-identical**:
lamp/dragon/lamp_absorption stdout (GPU), lamp stdout (CPU),
envtest/suzanne stdout, self_shadow PNG (CPU md5).

**1.2 Background color.** Parser: `"background": [r,g,b]` or
`{"color": [r,g,b]}` → `has_bg_color`/`bg_color[3]` (default unset →
env/procedural miss exactly as before). Miss order (both backends):
medium-with-σ → 0, else bg color, else env, else procedural. The three
dish scenes now set `"background": [0.0, 0.0, 0.0]` → black void like
the reference (corner/bottom pixels (0,0,0), both backends; gold
reflections unchanged).

## Current state

```sh
./ray2 test_scenes/scene_iri_dish.json            # GPU, seconds
./ray2 --cpu test_scenes/scene_iri_dish.json      # CPU, ~85 min at 1024x934
./ray2 --cpu test_scenes/scene_iri_dish_small_stdout.json  # 512x467, ~20 min
```

Scene: studio HDR @ intensity 1.0, env-only lighting (point lights
removed in Phase 2), no floor (floor-free since Phase 1) +
`"background": [0,0,0]` (black void), 1024×934 (matches the reference's
1280×1167 aspect).

- **CPU/GPU parity: RESOLVED (Phase 4).** The former 41.01%/max 109
  divergence was **not** the double-shell / medium-state hypothesis below — it
  was a GPU texture-binding limitation: the kernel bound only one baseColor,
  one ORM and one iridescence texture for the whole scene, so with 4 materials
  / 11 textures every mesh sampled the wrong maps (baseColor was pinned to
  `glassdish_irid.png` for all). Fixed by binding all scene textures as an
  argument-buffer `array<texture2d<float>, MAXTEX>` (MAXTEX=64) and indexing it
  per material by `tex_index / orm_tex_index / iri_tex_index`, exactly like the
  CPU (`gpu_renderer.mm`, `shaders.metal`). A second GPU bug fixed alongside:
  direct-light shadow rays passed the *triangle* index as `in_shadow`'s
  `skip_mesh` (which filters by `tris[].mesh_idx`, a *mesh* index). Result at
  256×234: the dish sits at the recorded **137 px / 0.23%** cross-backend floor
  (not byte-identical); the full lamp (768×1024) is **3.45%** cross-backend —
  the "byte-identical" reading recorded here was a CPU-vs-CPU false positive
  from the Metal page fault (fixed in 83d6230); lamp was previously 5.07%.
  Re-measured at 512×467: the former 98,053 px / 41.01% / sum 4,362,327 /
  max 109 is now at the 0.23% floor.
 - **CPU cost**: 61M primary-sample rays at 512×467 in ~20 min. BVH counters:
   olives mesh 1.78e9 tri-tests, gold 1.74e9 per frame — glass bounces
   re-traverse the BVH very heavily. Full-res CPU is ~85 min; keep parity
   checks ≤512 and use the GPU for looks.

## envtest CPU/GPU divergence fixed (2026-09-04)

The lone failing parity gate — `scene_envtest_stdout.json` recorded
`known-bug` (4.08% / max 255, a regression from the Phase 2 IBL commit) — is
now fixed and re-baselined to `ok` (0.13% / max 3). The no-arg gate PASSES.

**Root cause.** The GPU's prefiltered env path used
`env_tex.sample(sampler, coord, lod)` to pick the roughness-blurred mip level.
On this MSL (macOS 26.6 / Metal 4) that explicit-LOD overload samples the
**wrong level** — it returns sharp level-0 data instead of the requested blurred
level (the texture itself is correct; a `getBytes` read-back of level 10 matched
the CPU's 1×1 average exactly). The alternatives are unavailable in this MSL:
`sample_level`, `read(int2, level)`, and `sample(s, coord, grad)` all fail to
compile ("no matching member function"). So metallic/plastic IBL rays escaped to
the wrong (sharp) level while the CPU sampled the correct blurred one. Isolated
with a single-sphere scene: CPU gold sphere uniform (the level-10 average × F0
tint), GPU non-uniform (sharp).

**Fix.** The GPU now reads the CPU-built mip chain from a **device buffer** and
does the within-level bilinear by hand (`env_bilinear_read` in `shaders.metal`),
mirroring the CPU's `envmap.cc sample_level_data` operation-for-operation
(`u*w-0.5` texel-center offset, floor, wrap, 4-texel lerp). `gpu_renderer.mm`
packs the chain into the buffer (`gpu_create_env_mip_buffer`, level 0 first);
`SceneGpu` gained `env_w`/`env_h` (132→140 bytes, asserted both sides); the
buffer is bound at `buffer(12)`. The mipmapped texture is still used for the
sharp (primary-ray) path, which was never affected.

**Result.** envtest 4.08%/max 255 → **0.13%/max 3** (residual = the normal
float-rounding floor). All other scenes (dish, suzanne, dragon, lamp) unchanged
at their exact baselines — the change is isolated to the env-sampling path.

## Delta analysis (reference vs current render, ranked by visual impact)

1. **No IBL (biggest gap).** ~~The reference is lit by the studio
   environment: softbox highlights on the glass, the gold reading as gold,
   waxy diffuse olives. Our backends use the envmap **only as the
   primary-miss background**; ambient is flat `0.15 × color × AO`;
   specular is point lights only. With the floor covering the frame, the
   env wasn't even visible — the whole image is carried by 3 point lights
   + flat ambient. (Phase 1 removed the floor and set the black
   background, so the env is no longer even the backdrop — it now lights
   **nothing**: the IBL gap is fully exposed.)~~ **FIXED (Phase 2,
   2026-08-29):** prefiltered-env specular + band-0..1 SH diffuse
   irradiance on both backends (renderer.cc:546-556, 818-827;
   shaders.metal:779, 810); the dish scene is env-only now (point lights
   removed). See "Phase 2" above.
2. **The floor is unconditional.** ~~`has_floor` is parsed (parser.cc) but
   *never read by any renderer code*; `hit_floor` (CPU) and the `hf0` floor
   branch (GPU) always run. The blue checker dominates our background and
   reflects into the glass and olives.~~ **FIXED (Phase 1, 2026-08-26):**
   floor is opt-in behind `"floor"`, dish renders floor-free against the
   black `"background"`; all implicit-floor scenes carry the explicit key
   and are byte-identical to the pre-change binary.
3. **The gold plate is invisible** (it is the reference's main subject).
   Ours reads as a dark teal/maroon ring. Causes, in order: (a) the dish
   glass above it is too dark (item 4), (b) no IBL to carry the gold's
   diffuse + specular (item 1), (c) `goldleaf_nrm` (2048², strong weathering)
   is ignored (item 5). Note the ORM says metallic ≈ 0.098 (G channel = 25)
   — the gold look in the reference is *basecolor diffuse + IBL*, not
   metalness, which makes item 1 decisive.
4. **Dish glass too dark/green.** Reference: near-neutral glass with a
   rainbow band on the rim. Ours: uniform dark-green cast. Root cause: the
   **iridescence color lobe is not parsed** — `iridescenceTexture` (RGB)
   modulates the film; only `iridescenceThicknessTexture` (B) is parsed.
   Measured: `glassdish_irid.png` R channel mean 101/255 with dark regions
   (suppresses the film where the reference dish is neutral); G/B ≈ 218
   (thickness ≈ 543 nm, near-uniform). Second-order: the dish's standalone
   `occlusionTexture` (R of goldleaf_orm, real variation) is ignored
   (our AO only comes from the metallicRoughnessTexture R).
5. **Normal maps ignored.** `olives_nrm` (512², skin bumps), `goldleaf_nrm`
   (2048²), `glasscover_nrm` (**scale 2.0**, wavy ripples clearly visible on
   the reference dome as broken-up highlights). The reference dome's
   characteristic ripple pattern is the single most recognizable surface
   feature; ours is a smooth mirror.
6. **Cover iridescence too weak.** Reference dome: rich magenta/green/teal
   bands (thickness 500–515 nm via `glasscover_irid.png` B ≈ 74/255,
   R ≈ 252 = white, i.e. unsuppressed film). Ours: faint specks. Likely a
   combination of items 1+5 (the film tints *reflected environment* — with a
   dark floor and point lights there is little light to tint; the ripples
   spread the film response over angles) — verify the film model itself
   against the vendored three.js `evalIridescence`
   (`web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
   iridescence_fragment.glsl.js`) with a cover-normal probe before assuming
   a model gap.
7. **Olive "candy" look** (saturated teal/red mirrors). The ORM is parsed
   per spec (R=AO mean 142 with variation, G=metallic ≈ 0.6, B=roughness 0),
   so this is *not* a misread: the look is the blue floor + point lights in
   the tight F0/mirror lobes + no normal detail + no diffuse IBL. Expected
   to fall out of items 1–2 + 5; no ORM change planned.
8. **`alphaMode: MASK` not implemented.** goldleaf_col.png has 27,248 px
   (1.3%) with alpha < 128 (hairline cracks in the leaf); the sheet renders
   solid. Minor, but visible in the reference's cracked-leaf detail.
9. **`KHR_materials_volume` is inert here — no gap.** No
   `attenuationColor`/`attenuationDistance` (σ = 0), so thickness
   (factor 0.1 / texture) affects nothing in our tracer *or* in three.js's
   transmission absorption. Documented for completeness; nothing to do.
10. **Tone/exposure.** Reference is brighter and warmer; after IBL lands,
    sweep `exposure` (our Reinhard) against the reference's overall
    luminance.
11. **Cover pose.** t=0 pose matches the reference; the animation tilts the
    cover ~12° by t≈3.3 s. No action unless a mid-animation frame is wanted.
12. **CPU performance.** See current state; parity at full res is
    impractical on CPU by a wide margin.

## Plan (phased)

### Phase 1 — Scene foundation (cheap; unblocks fair comparison) — DONE 2026-08-26

1.1 **DONE: Gate the floor behind `has_floor`** — CPU `trace_ray`
(`SceneOpts` threaded through renderer.cc) and the GPU `hf0` branch
(shaders.metal). Blast radius was larger than originally noted: **10**
scenes relied on the unconditional floor (dragon ×3, lamp ×6, self_shadow)
— all carry `"floor": true` now; gate vs the pre-change binary: **0
differing pixels** on all of them (details in "Phase 1 landed" above).
1.2 **DONE: Background color option in scene JSON** —
`"background": [r,g,b]` and `{"color": [r,g,b]}` both parse; default =
env/procedural miss (unchanged). Primary miss returns the color instead
of the env; env stays available for IBL (Phase 2). Both backends. The
three dish scenes set `[0,0,0]` and render against black like the
reference (side-by-side regenerated).
1.3 **(Separate task, do not mix in):** re-download a real `haven_01` HDR or
switch the dragon to a real studio HDR; re-baseline the dragon render.

### Phase 2 — Image-based lighting (the big one) — DONE 2026-08-29

2.1 **DONE: Pre-filtered env for specular.** Mip chain of the env built at
load in `envmap.cc` (roughness→mip-level mapping);
`envmap_sample_prefiltered` samples linear-mip-linear along the reflection
direction. A specular/refracted ray escaping to the env sees it blurred by
the roughness of the surface that spawned it (the traced mirror ray is the
sampled lobe in the sharp limit — no separate env-lobe term to
double-count); primary rays keep the sharp env (renderer.cc:546-556).
GPU: the chain uploads as a mipmapped `MTLTexture` (`env_mips`) and
`sample_env_prefiltered` mirrors the CPU. Without a loaded env the
prefiltered path falls back to the sharp sample exactly (legacy).
2.2 **DONE: Diffuse irradiance.** Band-0..1 SH from the env at load (no
texture dependency); `envmap_irradiance` is the closed-form
`E(N) = √π·c00 + √(π/3)·(c1·N)`. CPU: with a loaded env the flat 0.15
ambient is replaced by `kd × irradiance(N) × AO × (1−transmission)`
(renderer.cc:818-827); without one the legacy flat ambient is kept exactly.
GPU: `has_env ? fl * env_irradiance_sh(ibl_sh, nf) : fl * 0.15f`
(shaders.metal:810). This three.js-parity-grade term is what gives the
olives/gold their waxy, studio-lit diffuse.
2.3 **DONE: Wiring.** Point lights remain (kept for scenes that need
them); the dish scene is now env-only — `"lights": []`, studio HDR @
intensity 1.0 — matching the reference's env-only lighting.
2.4 **DONE: Gate.** No env file → `has_env=0` / `!env->data` → IBL terms
are exactly zero (specular falls back to the sharp sample, diffuse to the
flat 0.15 ambient). **CPU half: PASSED 2026-08-29** — lamp 768×1024 and
dragon 1024×768 CPU renders at HEAD are **byte-identical (0 differing px,
sum_abs_err 0, max err 0)** vs 26223ae CPU renders (clean worktree build,
`tools/ppm_diff.py`); both scenes still point at the broken
`polyhaven_haven_01_1k.hdr`, which fails to load, so the no-env fallback
is what was exercised. **Limitation: the GPU half cannot be run as a
commit delta** — the Phase 4 GPU parity fix and the IBL code both landed in
ef63cfd, so no post-Phase-4 pre-IBL commit (hence no rendered GPU
baseline) exists, and 26223ae's GPU path predates the Phase 4
texture-binding fix, so a 26223ae-GPU vs HEAD-GPU diff would conflate the
two changes. Reference instead: the recorded post-Phase-4 gate (Phase
4.2) measured CPU-vs-GPU byte-identical — lamp 768×1024 **0 px**, dragon
1024×768 **0 px** — on a binary that already contained the IBL code; with
the CPU side now proven unchanged vs 26223ae, the GPU no-env path follows
by transitivity. [INVALID — pre-83d6230, CPU-vs-CPU: the Phase 4.2
"CPU-vs-GPU byte-identical" reading was a false positive from the Metal
page fault, so this transitivity step is unsound. Honest cross-backend
numbers: lamp 3.45%, dragon 5.94%.]

### Phase 3 — Normal maps (+ AO + MASK)

3.1 Parse `normalTexture` (index + scale) and standalone `occlusionTexture`
into `MeshObj`/`MeshObjData`/`MeshMatGpu`/`MeshMat` (plumbing pattern is
established by the transmission field; the static_assert grows again).
3.2 Tangent frame: these meshes carry no `TANGENT` attribute (verify in
`--mesh-stats`); use the UV-derivative screen-space TBN (GPU
`dfdx/dfdy`; CPU from per-triangle UV frame) with the scale factor —
three.js parity is not required here (scale 1.0/2.0, standard tangent-space
decoding: `N = TBN × (2×tex−1)`).
3.3 Apply in the unified PBR shading of both backends (diffuse normal AND
specular normal; AO on ambient+diffuse per the existing convention).
3.4 **`alphaMode: MASK`**: load baseColor with alpha (check stb channels
in the image loader), discard fragments below `alphaCutoff` (default 0.5)
in both backends (GPU: early exit in the hit loop is wrong — must push the
ray back into the trace; CPU: treat as a miss for that triangle).
3.5 Verification: normal-debug render (TBN colored) + re-diff the dome
against the reference's ripple pattern.

### Phase 4 — CPU/GPU parity chase — DONE 2026-08-26

4.1 **Localized — the medium-state/4-crossing hypothesis was WRONG.** Masked
diffs + per-channel signed stats showed the divergence was a broad,
whole-surface color error (CPU warm-gold vs GPU cyan, R≈0), not a
geometric/seam artifact. Root cause: the GPU bound a **single** baseColor/ORM/
iridescence texture for the entire scene (`gpu_renderer.mm` picked the first
texture with data per map type), so in this 4-material / 11-texture scene every
material sampled the wrong image. The CPU samples `textures[per-material index]`
freely. (The 4-crossing cap and `in_shadow` self-skip were red herrings for the
*dominant* term, though the shadow-skip *was* a real, separate bug — see 4.2.)
4.2 **Fixed + rebaselined.**
- `shaders.metal` + `gpu_renderer.mm`: bind every scene texture as an
  argument-buffer `struct TexBundle { array<texture2d<float>, MAXTEX> t; }`
  (MAXTEX=64) passed `const device&` at `[[buffer(10)]]`; the array is laid out
  *inline* (encodedLength = MAXTEX·8) so the host sets each slot directly on the
  argument encoder and the kernel samples `scene_tex.t[idx]` dynamically
  (`idx` = the material's `tex_index`/`orm_tex_index`/`iri_tex_index`), matching
  the CPU exactly. MSL rejects a bare `const device array<…>&` kernel arg and
  gives no sub-encoder for the array — it must be the inline array inside a
  struct argument buffer.
- `shaders.metal`: direct-light shadow rays passed the *triangle* index `mi` as
  `in_shadow`'s `skip_mesh`; now derive the mesh index
  (`tris[mi].mesh_idx`), matching the CPU and the emissive path.
Gate (CPU vs GPU, this binary): dish 256×234 **and 512×467 0 px**, lamp 768×1024
**0 px**
(masked glass-sphere region 0/0), dragon 1024×768 **0 px**, envtest unchanged
0.06%/max 2, suzanne 14.23%→13.84% (pre-existing procedural-path gap, unrelated
to textures, marginally improved). README parity table rebaselined.
[INVALID in part — pre-83d6230, CPU-vs-CPU: the "0 px" readings for
dish/lamp/dragon were false positives from the Metal page fault (all three
index the 64-slot bundle). Honest cross-backend numbers: dish 0.23%, lamp
3.45%, dragon 5.94%. The "envtest 0.06%" and "suzanne 13.84%" are NOT
page-fault false positives — neither scene carries material textures, so
the GPU was genuinely running; both are real CPU-vs-GPU readings (envtest
since superseded by 4.08%, suzanne by 13.88% at HEAD).]

### Phase 5 — Iridescence color lobe

5.1 Parse `iridescenceTexture` (RGB) into the material (new `iri_color_tex`
field, same plumbing pattern).
5.2 Multiply the thin-film F0 response per channel before the
`iri_factor` blend (glTF KHR_materials_iridescence: the RGB texture
modulates the iridescence effect). Expect: dish cast relaxes to the
reference's neutral glass + rim band; cover bands strengthen.
5.3 If the cover bands are still off after 1+2+3+5, probe the film model
against vendored three.js `evalIridescence` (citation in item 6) — parity
was established for the lamp (factor-only); this asset adds texture-driven
thickness + color.

### Phase 6 — Polish

6.1 Exposure sweep vs reference luminance (per-region mean histogram
comparison).
6.2 Update README feature table (normal maps, MASK, standalone AO,
iridescenceTexture, IBL) and rebaseline the lamp/dragon parity numbers.
6.3 Optional: cover mid-animation frame (a one-line keyframe lerp in the
parser if ever wanted).

## Verification recipes

```sh
# build
make

# GPU render (seconds) / CPU render (~20 min at 512x467, ~85 min at 1024x934)
./ray2 test_scenes/scene_iri_dish.json
./ray2 --cpu test_scenes/scene_iri_dish_small_stdout.json > /tmp/dish_cpu.ppm
./ray2 test_scenes/scene_iri_dish_small_stdout.json        > /tmp/dish_gpu.ppm

# CPU/GPU parity. NOTE: the "Phase 4 resolved: dish 256x234 / lamp / dragon
# are byte-identical" reading was a pre-83d6230 CPU-vs-CPU false positive from
# the Metal page fault. Honest cross-backend numbers: dish 0.23%, lamp 3.45%,
# dragon 5.94%.
python3 tools/ppm_diff.py /tmp/dish_cpu.ppm /tmp/dish_gpu.ppm

# env path sanity. NOTE: the envtest scene carries no material textures, so
# it did NOT index the 64-slot bundle and was NOT masked by the Metal page
# fault. The old 4.08% / max 255 divergence (the GPU's broken explicit-LOD
# env sample) is fixed 2026-09-04 — the honest HEAD number is now ~0.13% /
# max 3 (the float-rounding floor); see "envtest CPU/GPU divergence fixed".
./ray2 --cpu test_scenes/scene_envtest_stdout.json > /tmp/e_cpu.ppm
./ray2 test_scenes/scene_envtest_stdout.json        > /tmp/e_gpu.ppm
python3 tools/ppm_diff.py /tmp/e_cpu.ppm /tmp/e_gpu.ppm

# material plumbing check
./ray2 --cpu test_scenes/scene_iri_dish.json --mesh-stats 2>&1 | grep '\[gltf:mat\]'
# expect: idx=0 transmission=1 iri=1 iri_nm=[500,550] iri_tex=0
#         idx=2 transmission=1 ior=1.500 iri_tex=6 vol_th=0.1 vol_tex=5

# side-by-side (reference must be PNG — convert the JPG once:
# python3 -c "from PIL import Image; Image.open('test_scenes/IridescentDishWithOlives/screenshot_Large.jpg').save('/tmp/iri_ref.png')")
python3 tools/sidebyside.py /tmp/iri_ref.png /tmp/dish_gpu.ppm images/iri_dish_sidebyside.png

# control gate (floor gating, Phase 1.1): PASSED 2026-08-26 — lamp,
# dragon and lamp_absorption stdout (GPU), lamp stdout (CPU),
# envtest/suzanne stdout, self_shadow PNG (md5) all byte-identical
# vs the pre-change binary (/tmp/ray_pre_phase1) after adding the
# explicit floor key
#
# control gate (IBL, Phase 2.4): PASSED 2026-08-29, CPU half — lamp
# 768x1024 and dragon 1024x768 CPU renders byte-identical (0 px, sum 0,
# max 0) vs 26223ae (clean worktree build; both scenes' broken haven_01
# HDR still fails to load, so the no-env fallback was exercised).
# GPU half not runnable as a commit delta: no post-Phase-4 pre-IBL commit
# exists (IBL and the Phase 4 GPU fix both landed in ef63cfd); referenced
# from the recorded Phase 4.2 CPU-vs-GPU 0 px numbers instead.
# NOTE: that reference is unsound — the Phase 4.2 "CPU-vs-GPU 0 px" numbers
# were a pre-83d6230 CPU-vs-CPU false positive from the Metal page fault, so
# the GPU no-env path does NOT follow by transitivity. Honest cross-backend
# numbers: lamp 3.45%, dragon 5.94%.
# git worktree add /tmp/ray-26223ae 26223ae && make -C /tmp/ray-26223ae
# /tmp/ray-26223ae/ray2 --cpu test_scenes/scene_lamp_stdout.json > /tmp/l2.ppm
# ./ray2 --cpu test_scenes/scene_lamp_stdout.json > /tmp/l1.ppm
# python3 tools/ppm_diff.py /tmp/l2.ppm /tmp/l1.ppm   # 0 px
# (same pair for scene_dragon_stdout.json)
```

Path gotchas (inherited): relative `gltf` paths resolve against the *scene
file's directory*; `environment.file` resolves against the *CWD* (run from
the repo root). `--mesh-stats` enables parse diagnostics but still renders —
capture the parse output and kill the process, do not wait for the frame.
**Makefile gotcha (learned 2026-08-26): no header dependency tracking** —
after editing `include/scene.h` (or the SceneGpu structs) always
`make clean && make`; an incremental build links stale objects against the
new layout. Also: a scene's `"output"` key diverts stdout (PNG to file +
a one-line message), and both backends print a prefix (`GPU` / `28T`)
before the stdout PPM — `ppm_diff.py` scans for `P6\n`, plain byte
comparison of two GPU renders must strip the prefix first.
