# Dragon Attenuation — bug writeups and next steps

Scene: `test_scenes/scene_dragon.json` →
`test_scenes/DragonAttenuation/DragonAttenuation_mirrored.gltf`
(glTF-Sample-Models `DragonAttenuation`: KHR_materials_transmission +
KHR_materials_volume on a solid single-shell Stanford dragon).

**Reference:** `images/dragon_attenuation_reference.png` — big amber glass dragon
filling the frame, head left, wavy checker cloth behind, blue checker floor.
Side-by-side: `images/dragon_sidebyside.png` (regenerate after each change).

Render commands: `./ray2 --cpu test_scenes/scene_dragon.json` (CPU) /
`./ray2 test_scenes/scene_dragon.json` (GPU), 1024×768, `AA_SAMPLES 16`
(256 spp/px), `MAX_DEPTH 4`, 4 point lights, env
`envmaps/polyhaven_haven_01_1k.hdr` @ intensity 7.0.

## Current state (2026-08-25)

- **Phase 1 (shared in-medium surface-term bug): FIXED** — the dragon is no
  longer a milky-white blob; it renders amber. Control gate byte-identical,
  CPU/GPU parity measured.
- **Phase 2 (fidelity vs reference): FIXED** — mirrored gltf repaired (2.1),
  transmission replaces diffuse (2.2), camera re-framed (2.3), light levels
  tuned (2.4). The render now matches the reference: deep amber glass, head
  left, thin spikes bright / body dark, wavy checker backdrop, blue checker
  floor. Side-by-side: `images/dragon_sidebyside.png`.

Working-tree state at this writing (nothing committed for the dragon work yet):

| File | Change |
|------|--------|
| `src/renderer/renderer.cc` | Phase 1 CPU fix (Tseg charged to surface terms); Phase 2 glass diffuse × (1−transmission) |
| `src/renderer/shaders.metal` | Phase 1 GPU fix; Phase 2 same (MeshMat gains `transmission`) |
| `src/parser/gltf_parser.cc` | Phase 1 checker fallback excluded for glass/emissive; Phase 2 plumbs `transmissionFactor` into `MeshObj` |
| `include/scene.h`, `include/types.h`, `src/renderer/gpu_renderer.mm` | Phase 2: `transmission` field on MeshObj/MeshObjData/MeshMatGpu (static_assert 104→108) |
| `test_scenes/DragonAttenuation/`, `test_scenes/scene_dragon*.json` | Scene assets; mirrored gltf node transforms repaired (2.1); camera + env tuned (2.3/2.4) |

---

## Phase 1 — In-medium surface terms escaped attenuation (shared bug) — FIXED

**Symptom:** the dragon rendered as a milky-white/gray blob with the checker
floor visible straight through the body. Same on CPU and GPU (shared bug — a
parity diff would never have shown it).

**What was already correct** (ruled out via a temporary per-hit medium tracer
on `trace_ray`, since removed):

- Node transforms bake in correctly (dragon bbox upright
  x∈[−1.76, 1.76] y∈[−0.73, 1.75] z∈[−0.79, 0.79]).
- Material plumbing exact (`--mesh-stats`: transmission 1.0, ior 1.5 [no
  KHR_materials_ior in the file → spec default], `vol_th` 2.27,
  attenuationColor (0.921, 0.64, 0.064), attenuationDistance 0.155 →
  σ = (0.531, 2.879, 17.735) per unit).
- Stored normals point outward (mean dot(normal, radius-vector) +0.536 over the
  76,809 dragon verts), so `hit_mesh_bvh`'s pre-flip `side` classification
  (front = enter the volume, back = exit) is sound.
- The mesh is a **single shell** (not double-walled): probe rays through the
  body show 4–8 crossings (body tube looping), not an inner surface; body tube
  diameter ≈ 0.3–0.6 world units.
- Measured in-medium chords 0.05–0.41 units; entry sets the medium, exit
  clears it, and `refl`/`refr` continuations were charged with `Tseg` exactly
  once per segment, as designed.

**Root cause:** `trace_ray` (CPU) / the Metal trace loop (GPU) charge the
Beer–Lambert transmittance of the segment just traveled (`Tseg`) onto the
*downstream* reflection and refraction, but **not** onto the hit's surface
term (`ambient + lit`). For a solid single-shell body that term is the light
on the **far wall** (the back-face hit): that light must travel the full chord
back through the medium to reach the camera, so it must carry `Tseg`. It
leaked unattenuated — white surface light from the 4 lights + ambient, ×
baseColorFactor (1,1,1,1) — washing the amber transmission out.

Why the Phase-3 lamp work never saw it: the lamp's glass is a two-surface thin
wall (gap ≈ 0.004 units), so the leaked chord's `T ≈ 0.98` — negligible. The
dragon's chords are 10–100× longer, so the leak is the whole image.

**Fix (both backends):** compute `Tseg` at hit resolution (before any terminal
contribution) and charge every contribution leaving the surface back toward
the camera:

- CPU `renderer.cc`: `Tseg` moved up to just after the hit point is computed
  (~line 531); `base_color *= Tseg` (covers the `MAT_SUBSURFACE` early
  return), the floor-hit `lit` return, and the `MAT_EMISSIVE` return each
  carry it; the pre-existing `refl_col *= Tseg` / `refr_col *= Tseg` bakes are
  unchanged.
- GPU `shaders.metal`: `Tseg` moved up to just after `float3 p = …`
  (~line 713); `accum += lit * (thru * Tseg)` (floor),
  `accum += sc_col * (thru * Tseg)` (emissive), `accum += base * (thru * Tseg)`
  (surface term). The stack-push / `thru` continuations already carried `Tseg`.

The physical reading: the camera ray *is* the light path reversed, so every
segment the ray travels in the medium is a segment the light travels in the
medium; the surface term at an in-medium hit is light that starts at that hit
and must exit the medium, so it takes the segment already traveled. (Direct
light *arriving at* an in-medium hit is a separate question — shadow rays do
not attenuate; see Known Limitations.)

**Companion fix (same work, `gltf_parser.cc`):** the procedural-checker
fallback (for UV verification on textureless meshes) now excludes
`glass`/`emissive` — otherwise the dragon's transmission material got painted
with a white checker instead of using its `baseColorFactor`.

**Verification:**

- **Control gate (σ-gate by construction):** `Tseg ≡ (1,1,1)` unless the ray
  is in a medium with σ ≠ 0, so all existing scenes are untouched. Verified:
  canonical lamp scene (`test_scenes/scene_lamp.json`, 768×1024), git-HEAD
  build (`git archive HEAD` in a temp dir) vs the new build — CPU
  **0** differing px / sum_abs_err 0 / max 0, GPU **0** / 0 / 0 —
  byte-identical, both backends.
- **CPU/GPU parity after the fix** (dragon scene, 1024×768, stdout-PPM
  method, `tools/ppm_diff.py`): **62,633** differing px (7.96%) /
  sum_abs_err 305,406 / max channel err **8** — the documented
  CPU-recursive-vs-GPU-iterative glass-traversal class (the lamp canonical
  scene runs ~16% / max 86 over a far larger glass area), no new divergence
  class.

---

## Phase 2 — Fidelity gaps vs the reference — FIXED (2026-08-25)

### 2.1 `DragonAttenuation_mirrored.gltf` was broken (asset bug) — FIXED

The mirror's intent was to flip the dragon left/right to match the reference
(reference: head left; original gltf: head right). What it actually did:

- **Dragon rotation `(0, 0, 0.7071, −0.7071)`** — a 90° rotation about **Z**
  instead of the original 90° about **X** (`(0.7071, 0, 0, 0.7071)`). It tips
  the dragon onto its side: bbox y∈[−2.49, 1.03] — most of the body below the
  floor plane. (The renderer's floor is unconditional — `hit_floor` in
  `renderer.cc` runs with no `has_floor` gate — so the submerged part is
  simply hidden.)
- **Cloth node scaled 3.5 → 12** with the translation removed. The backdrop
  cloth then spans z∈[−5.5, +17.1] and swallows the camera at z=12 (the
  glossy "floor" in the broken render is the cloth draped in front of the
  camera).
- The `_mirrored` file shares `DragonAttenuation.bin` (5,817,396 bytes) — it
  does **not** flip normals/winding; it only edits node transforms.

**Fix applied (gltf edit only, no renderer change):** dragon node rotation is
now the original X-90° composed with a 180° yaw about Y (the L/R flip):

    "rotation": [0.0, 0.7071067811865476, -0.7071067811865476, 0.0]

and the **original** cloth node is restored (scale 3.5, translation
`[-0.15470129251480103, -0.841584324836731, -0.1703687310218811]`).

**Verified:** the file differs from the original in `nodes` only (all other
top-level keys byte-identical JSON). Transformed dragon bbox is the original's
with x flipped: x∈[−1.762, 1.762] y∈[−0.731, 1.754] z∈[−0.788, 0.788]; the
topmost vertex (head) moves from x=−0.483 (original) to x=+0.483… i.e. the
whole pose is the exact x-mirror of the original, head now left, matching the
reference. (Cloth note: its wavy front edge reaches z≈+4.83 — keep the camera
outside that, see 2.3.)

### 2.2 Glass diffuse is not replaced by transmission (model difference) — FIXED

three.js (the reference's renderer — source of record, vendored in
`web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
transmission_fragment.glsl.js:33`):

    totalDiffuse = mix( totalDiffuse, transmitted.rgb, material.transmission );

For a transmission=1 material the white baseColor diffuse is **removed** and
the attenuated transmitted light takes its place. Our tracer added the *full*
white diffuse (`lit += sc × N·L` in the `MAT_GLASS` branch,
`renderer.cc` / `shaders.metal`) on top of the transmission — residual pale
peach instead of the reference's deep amber/dark-red-brown.

**Fix applied:** `transmissionFactor` is now plumbed end to end —
`gltf_parser.cc` → `MeshObj` (`include/scene.h`) → `MeshObjData`
(`include/types.h`, copied in `setup_context`) → `MeshMatGpu`
(`gpu_renderer.mm`, `static_assert` 104→108) → `MeshMat`
(`shaders.metal`). In both backends every **diffuse** contribution for
`MAT_GLASS` meshes is scaled by `(1 − transmission)`: the direct-light
diffuse, the ambient term, and the emissive-surface diffuse. The specular
lobe and the F0 mirror are untouched (three.js keeps them). JSON glass
spheres carry no transmission factor (0) and are unchanged; non-glass meshes
see `glass_trans = 0` and are bit-identical.

**Blast radius (measured, as anticipated):**

- **Lamp canonical scene** (`scene_lamp_stdout.json`, 768×1024): the glass
  sphere (transmission 1.0) goes from milky-white-veiled to clear glass —
  the intended three.js behavior. New CPU/GPU parity baseline:
  **39,875** differing px (5.07%) / sum_abs_err 275,379 / max channel err
  **80** (old baseline 126,677 / 8,149,177 / max 86 — removing the in-glass
  surface diffuse also shrank the CPU-recursive-vs-GPU-iterative divergence).
  Gate-vs-new diff is confined to the glass: 110,021 of 110,184 differing px
  (99.85%) fall inside `test_scenes/lamp_glass_mask.ppm`; the 163 px outside
  are max-err-2 mask-edge AA. Shade and base unchanged.
- **Plastic control (Suzanne, `scene_suzanne_stdout.json`):** git-HEAD build
  vs new build — **0** differing px, byte-identical. P1+P2 are no-ops off
  glass.

### 2.3 Camera framing — FIXED

`scene_dragon.json` now sits at **`(1.1, 0.75, 5.2)`** looking at
**`(0, 0.5, 0)`**, fov 30 — roughly 5.3 units from the dragon, slightly above
its mid-height (y∈[−0.73, 1.75]), with a small positive-x azimuth that
matches the reference's viewing angle (tail curling toward the viewer on the
right). The dragon fills ~85% of frame height like the reference. Scene-file-
only change.

**Gotcha:** the cloth's wavy front edge reaches z≈+4.83 (BVH root bbox
z∈[−1.78, 4.83]) — the old z=12 camera was fine, but any camera closer than
~5 units on the +z side risks entering the cloth.

### 2.4 Light levels — FIXED

env intensity **4.0 → 7.0**; the 4 point lights are unchanged. The earlier
"too bright" judgment was made against the pre-2.2 pale-peach render; with
the transmission model in place the scene read darker than the reference
(cloth and floor in particular). At 7.0 the cloth/floor brightness and the
dragon's deep-amber body match the reference's soft studio look. Judged
against the side-by-side at the final framing.

---

## Known limitations (carried into the dragon scene)

- **Direct light arriving at an in-medium hit is not attenuated.** Shadow rays
  are visibility-only and `in_shadow` skips the hit mesh's own triangles, so
  the 4 lights fully light the dragon's far wall as if the glass weren't
  there. Physically, light from a camera-side light to a far-wall point should
  be attenuated by the chord. Three.js's rasterizer model doesn't do this
  either (its lighting never sees the ray), so this is deferrable for
  reference parity; it would matter for a light *inside* the glass.
- **`thicknessTexture` parse-and-report only** (Phase-3 judgment):
  `vol_tex_index` is stored and printed by `--mesh-stats` but not sampled.
  The reference's per-pixel thickness variation (the bright thin fins vs dark
  body) comes from real ray-traced chords here, not the texture — the texture
  matters only if we switch to a rasterizer-style thickness model.
- **Single-slot medium** (inherited): one absorbing medium per path; nested
  volumes unresolved. Vacuous for this scene (one volume).
- **Tessellation seams:** at entry seams a neighbor triangle's back face
  ~1e-3 behind the hit can flip the medium for one segment (documented in
  `nextsteps.md` Phase 3 item 4); the charge there is ~1 and unchased.

## Verification recipes

**Path gotcha (learned the hard way):** relative `gltf` paths in a scene file
resolve against the *scene file's directory* (`parser.cc` — no CWD fallback).
A copy of the scene in `/tmp` silently renders an **empty** scene (fopen
fails; floor + env only — the meshes never load). Always keep stdout variants
in `test_scenes/` (the `*_stdout.json` pattern).

```sh
# build
make

# CPU/GPU parity on the dragon (stdout PPM, diff tool scans for 'P6\n')
./ray2 --cpu test_scenes/scene_dragon_stdout.json > /tmp/dragon_cpu.ppm
./ray2 test_scenes/scene_dragon_stdout.json        > /tmp/dragon_gpu.ppm
python3 tools/ppm_diff.py /tmp/dragon_cpu.ppm /tmp/dragon_gpu.ppm
# current (final camera/env): 126,667 px (16.11%) / sum 1,562,690 / max 103 —
# the documented CPU-recursive-vs-GPU-iterative glass-walk class (glass fills
# the frame at the close-up camera; the lamp canonical runs ~5% / max 80)

# control gate (any scene without glass/attenuation must be byte-identical
# to the pre-change build; Suzanne is the plastic control, lamp the glass one)
rm -rf /tmp/ray_gate && mkdir /tmp/ray_gate
git archive HEAD | tar -x -C /tmp/ray_gate && make -C /tmp/ray_gate
# …render the same scene with both binaries, ppm_diff → expect 0 diff (Suzanne)

# material plumbing check
./ray2 --cpu test_scenes/scene_dragon.json --mesh-stats 2>&1 | grep -E '\[gltf:mat\]|\[mat\] mesh_idx=0'
# expect: transmission=1.000, att=(0.9210,0.6400,0.0640), att_d=0.155

# side-by-side vs reference (pure-python, no PIL needed)
python3 tools/sidebyside.py images/dragon_attenuation_reference.png \
    /tmp/dragon_cpu.ppm images/dragon_sidebyside.png
```
