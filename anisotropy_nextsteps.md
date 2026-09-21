# Anisotropy Barn Lamp — plan & findings (2026-09-11)

Ingested `AnisotropyBarnLamp` from Khronos glTF-Sample-Assets (Wayfair barn
light, showcase model for `KHR_materials_anisotropy`). Assets live in
`test_scenes/AnisotropyBarnLamp/` (unpacked from the upstream `.glb`);
gate scene is `test_scenes/scene_anisotropy_stdout.json` (768x876, camera
`pos [-0.16,-0.13,0.68]`, `target [0,-0.045,0.11]`, fov_y 32). Reference
render + photos: `images/AnisotropyBarnLamp_ref.jpg` (glTF Sample Viewer,
Studio Neutral env + ACES tone map).

Parity: CPU-vs-GPU certified `ok` (signature in `tools/parity_baselines.txt`).
Re-certified after the emissive_strength fix and again after the aniso port
(2026-09-11, now 17333/42719/27/13/0); max/p999 sit above the legacy floors
because this scene has more grazing glass/glass-against-sky pixels than the
legacy set, but it is under the cert bar — `ok`, not `known-bug`.

## What the model needs vs what we have

| Extension | Where | Status |
|---|---|---|
| KHR_materials_anisotropy (strength 1 + texture + rotation 0) | shade, mount arm | done (2026-09-11, both backends — see gap 1) |
| KHR_materials_clearcoat (0.25 / rough 0.15 + normal tex) | shade, back plate | direct lobe + energy done (Stage 1, both backends); IBL + clearcoatNormal open |
| KHR_materials_emissive_strength (25x) | filament | done (2026-09-11: factor × strength at load) |
| KHR_materials_transmission + volume (thick 0.01) | glass bulb | done |

## Gaps / bugs, in priority order

1. **Anisotropic specular — RESOLVED 2026-09-11.** Ported the three.js r169
   aniso path (same lineage as the KHR spec pseudo-code), mirrored op-for-op
   in `renderer.cc` + `shaders.metal`. Direct light: aniso GGX `F·V·D`
   replaces Blinn-Phong in a per-light `aniso > 0` branch (alphaT stretched
   along the anisotropy tangent, alphaB = rough²). Mirror/IBL: the mirror ray
   reflects about a bent normal stretched along the bitangent (Filament's
   single-sample PMREM trick); escaped rays keep sampling the prefiltered
   env at surface roughness. Texture semantics (corrected — the earlier
   "G = strength, B = angle" note was wrong): RG = tangent-space direction
   `normalize(2·rg−1)` rotated by `anisotropyRotation`, B = strength
   multiplier. No tangent plumbing was needed — `TriGpu` already carried
   per-vertex tangents (TANGENT accessor or MikkTSpace); the shaders
   interpolate them. CPU samples the aniso texture with `sample_linear3`
   (manual bilinear) vs GPU hardware bilinear — the usual accepted parity
   floor source. All new code is gated on `aniso > 0` / `aniso_factor > 0`:
   CPU stayed byte-identical on the full gate set (0 differing px, all
   three scenes) and the gate PASSes ≤ baselines; the aniso scene's
   CPU-vs-GPU signature was re-baselined to `ok`. The shade now shows the
   streaked brushed-copper highlight — the scene is no longer a "plausible
   metal" render.
2. **emissive_strength — RESOLVED 2026-09-11.** Parser now reads
   `KHR_materials_emissive_strength` and multiplies it into the emissive
   factor at load (filament: (1, 0.5, 0.25) → ×25). Radiance >1 flows
   unclamped through both backends (CPU `MAT_EMISSIVE` direct-glow +
   emissive-light sampling in `renderer.cc`; same paths in
   `shaders.metal`), so no shader mirror was needed — parser-only change.
   Default strength 1.0 keeps every other scene bit-identical (verified:
   full gate set PASSes with signatures unchanged to the digit).
3. **Clearcoat — direct lobe LANDED (2026-09, Stages 0–1).** Loader parses
   `KHR_materials_clearcoat` (factor/roughness/normalTexture) plumbed through
   `MeshObj`/`MeshMatGpu`/`MeshMat` (struct 144→156, paired asserts). Stage 1
   adds the direct GGX lobe (`f0=0.04`, `alpha=ccRough²`) + the
   `outgoing·(1−cc·Fcc) + cc·lobe` composite, mirrored op-for-op CPU↔GPU
   (`ccN` = non-perturbed normal). Gate green; non-coat scenes byte-identical
   (cc>0 gate airtight); aniso rebaselined `ok` (p99_9=14, n_severe=0).
   **Still open:** Stage 2 IBL (prefiltered PMREM tap × DFGApprox
   `EnvironmentBRDF`, both backends — the higher-parity-risk surface) and
   Stage 3 `clearcoatNormalTexture` (reuse the existing TBN).
4. **No GLB container.** Had to unpack the `.glb` by hand into
   `.gltf` + `.bin` + 4 PNGs (kept in `test_scenes/AnisotropyBarnLamp/`).
   A GLB reader (12-byte header, JSON/BIN chunks, embedded buffer + images)
   removes a manual step for every future Khronos sample.
5. **Env background seam.** The HDR background shows hard diagonal/horizontal
   seams (visible in every render of this scene, worst top-left). Separate
   from parity (the gate compares CPU vs GPU, both showing the same seam);
   looks like a panorama/mip sampling artifact in the background path, not a
   material bug. Reference uses a smooth studio backdrop, so this scene will
   never *match* the reference until this is investigated — but note the
   reference env is Studio Neutral, not `studio_small_03`.
6. **Tone map / exposure gap (by design, worth stating).** Reference is
   ACES-filmic; we render linear. The reference's copper warmth is partly
   ACES desaturation + the env's neutral tint. Don't chase color match by
   fudging materials before 1–3 are in.

## Non-gaps (verified)

- ORM + baseColor + normal textures load and sample on this asset (the
  "ROTATED / AnisotropyBarnLamp" print on the socket proves baseColor
  sampling; chrome-look is mostly missing-aniso, not missing-texture —
  baseColor mean is a gray-copper ~(125,133,120)).
- `extensionsRequired` is absent upstream, so the parser's
  fall-back-to-plastic warning path never fires; unsupported extensions are
  silently ignored, which is why the scene renders "plausible" today.
