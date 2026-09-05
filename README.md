# Ray Tracer (Refactored)

Componentized ray tracer with clean separation of concerns.

## Build

```bash
make          # Build
make clean    # Remove build artifacts
make run      # Build and run with default scenes/scene.json
make test     # Render all test scenes
```

## Usage

```bash
./ray2 scenes/scene.json              # Output PPM to stdout, PNG to file
./ray2 scenes/custom.json > image.ppm  # Override output to stdout
./ray2 --cpu scenes/scene.json        # Force CPU rendering
./ray2 --mesh-stats scenes/scene.json # Run diagnostics (geometry, BVH, materials)
```

The glTF test scenes (lamp, dragon, dish, suzanne, …) live in
`test_scenes/`, not `scenes/`; run them from the repo root (relative
`gltf` paths resolve against the scene file's directory, but
`environment.file` resolves against the CWD).

## Project Layout

```
├── scenes/              # Scene JSON files
├── models/              # OBJ mesh files
├── images/              # Rendered output images (only images/reference/ is tracked)
├── videos/              # Rendered animation videos
├── src/
│   ├── main.cc          # Entry point, animation loop
│   ├── parser/          # JSON scene + OBJ + glTF parsing
│   ├── renderer/        # Ray tracing core (CPU + Metal GPU)
│   ├── shading/         # Material properties, floor pattern
│   ├── vector/          # 3D vector math
│   ├── output/          # PPM/PNG writers
│   ├── denoiser/        # Image denoiser
│   └── envmap/          # HDR environment map loading
├── include/             # Shared type definitions + thin_film.h (CPU/MSL source of record)
├── tools/               # Mesh generators (torus, ico sphere, vase);
│   #                    # ppm_diff.py CPU/GPU parity harness; iridescence parity checks
├── web_viewer/          # Three.js-based glTF viewer (npm run dev)
├── test_scenes/         # glTF test models + scene JSON files
└── envmaps/             # HDR environment maps
```

## Features

| Feature | Status |
|---------|--------|
| CPU + Metal GPU backends | Done |
| BVH-accelerated mesh rendering | Done |
| Spheres, meshes, infinite floor (`"floor": true` / `{"checkerboard": true}`, opt-in) | Done |
| Scene background color (`"background": [r,g,b]` or `{"color": [r,g,b]}`; miss returns it instead of env/procedural) | Done |
| Materials: plastic, metallic, glass, emissive, subsurface | Done |
| Texture mapping (procedural: checker, polka, marble, rings) | Done |
| UV-mapped textures (OBJ + glTF path) | Done |
| Area light sampling (point lights with size) | Done |
| Emissive sphere + mesh lights | Done |
| Depth of field | Done |
| HDR environment maps | Done |
| Denoiser | Done |
| Animation (orbit camera) | Done |
| **glTF 2.0 importer** (core spec) | Done |
| KHR_materials_transmission (glass) | Done |
| KHR_materials_ior | Done |
| KHR_materials_volume (Beer–Lambert absorption) | Done (ray-traced path length, both backends) |
| Punctual lights (KHR_lights_punctual) | — |
| KHR_materials_iridescence (thin film, three.js parity model) | Done (both backends) |
| glTF baseColorTexture (sRGB→linear, bilinear) | Done |
| glTF ORM roughness (G channel × factor) | Done (linear sampling both backends) |
| Unified per-pixel PBR (unified plastic/metallic, ORM.B → metallic, F0 = mix(0.04, basecolor, metallic)) | Done |
| glTF AO (ORM.R) on ambient + per-light diffuse | Done (specular/mirror deliberately un-AO'd) |
| **Configurable FOV** (`fov_y` in scene JSON, default 90°) | Done |
| **Image texture loading** (PNG/JPEG via stb_image) | Done |
| **sRGB→linear conversion + bilinear sampling** | Done |

## Debug Flags

| Flag | Effect |
|------|--------|
| `--cpu` | Force CPU rendering path |
| `--mesh-stats` | Print geometry, BVH, material, and intersection diagnostics |
| `--threads N` | Set CPU thread count |

## CPU/GPU Parity Harness

CPU/GPU correctness is measured, not eyeballed: `tools/ppm_diff.py` plus the
committed canonical mask `test_scenes/lamp_glass_mask.ppm` (the glass-sphere
region of the IridescenceLamp scene — force-added since `*.ppm` is gitignored;
use the committed file, do not regenerate it). Reproduce the AE baseline:

```bash
./ray2 --cpu test_scenes/scene_lamp_stdout.json > /tmp/lamp_cpu.ppm  # CPU path
./ray2 test_scenes/scene_lamp_stdout.json > /tmp/lamp_gpu.ppm        # GPU path
python3 tools/ppm_diff.py /tmp/lamp_cpu.ppm /tmp/lamp_gpu.ppm
python3 tools/ppm_diff.py /tmp/lamp_cpu.ppm /tmp/lamp_gpu.ppm \
        test_scenes/lamp_glass_mask.ppm   # adds inside/outside masked split
```

`scene_lamp_stdout.json` is `scene_lamp.json` without its `"output"` key, so
both renders write raw PPM to stdout (a `28T`/`GPU` prefix line precedes the
PPM header; the diff tool scans for `P6\n`). Both backends are
byte-deterministic, so the counts are exact, not a noise level. Current
baseline (768×1024): **3.45%** cross-backend (CPU vs GPU). The earlier
"byte-identical" reading was a CPU-vs-CPU false positive from the Metal page
fault (fixed in 83d6230).
This landed with two GPU fixes: (1) a per-material texture array — the kernel
now samples `textures[tex_index / orm_tex_index / iri_tex_index]` from an
argument-buffer `array<texture2d<float>, MAXTEX>` (MAXTEX=64) that mirrors the
CPU's free per-material indexing, replacing the old single-texture-per-map-type
binding that only ever matched the first textured mesh; (2) a direct-light
shadow-ray fix in `shaders.metal` where the triangle index was passed as
`in_shadow`'s `skip_mesh` (which filters by `tris[].mesh_idx`, i.e. mesh index).
The IridescentDishWithOlives 4-material / 11-texture case — previously ~41%
divergent — is likewise byte-identical now. (Rebaseline history in
nextsteps.md.)

## Next Steps

**Next work item:** [glass_parity_nextsteps.md](glass_parity_nextsteps.md) —
close the glass light-transport CPU/GPU divergence (recursive vs iterative
traversal) driving the lamp (3.45%) and dragon (5.94%) parity floors. The
envtest bug (the last `known-bug`) is fixed; the full gate is green.

Other plan documents, newest first:

- [iridescent_dish_nextsteps.md](iridescent_dish_nextsteps.md) — the active
  plan, anchored on **IridescentDishWithOlives** (IBL, normal maps, MASK,
  iridescence color lobe). Phase 1 (opt-in floor + background color) landed.
- [dragon_nextsteps.md](dragon_nextsteps.md) — DragonAttenuation record:
  in-medium surface-term fix, transmission model, framing.
- [nextsteps.md](nextsteps.md) — the IridescenceLamp history (PBR
  foundation, iridescence, volume, Phase 4 refraction fix), the full AE
  rebaseline log, and the medium/longer-term list.
