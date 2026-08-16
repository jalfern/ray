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

## Project Layout

```
├── scenes/              # Scene JSON files
├── models/              # OBJ mesh files
├── images/              # Rendered output images
├── videos/              # Rendered animation videos
├── src/
│   ├── main.cc          # Entry point, animation loop
│   ├── parser/          # JSON scene + OBJ + glTF parsing
│   ├── renderer/        # Ray tracing core (CPU + Metal GPU)
│   ├── shading/         # Material properties, floor pattern
│   ├── vector/          # 3D vector math
│   └── output/          # PPM/PNG writers
├── include/             # Shared type definitions
├── tools/               # Mesh generators (torus, ico sphere, vase)
├── web_viewer/          # Three.js-based glTF viewer (npm run dev)
├── test_scenes/         # glTF test models + scene JSON files
└── envmaps/             # HDR environment maps
```

## Features

| Feature | Status |
|---------|--------|
| CPU + Metal GPU backends | Done |
| BVH-accelerated mesh rendering | Done |
| Spheres, meshes, infinite floor | Done |
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
| KHR_materials_volume (thickness) | Parsed, unused |
| Punctual lights (KHR_lights_punctual) | — |
| KHR_materials_iridescence | Not implemented (Phase 2 below) |
| glTF baseColorTexture (sRGB→linear, bilinear) | Done |
| glTF ORM roughness (G channel × factor) | Done (CPU/GPU parity bug — nextsteps.md) |
| glTF per-pixel metallic (B) / AO (R) | Not done (Phase 1 below) |
| **Configurable FOV** (`fov_y` in scene JSON, default 90°) | Done |
| **Image texture loading** (PNG/JPEG via stb_image) | Done |
| **sRGB→linear conversion + bilinear sampling** | Done |

## Debug Flags

| Flag | Effect |
|------|--------|
| `--cpu` | Force CPU rendering path |
| `--mesh-stats` | Print geometry, BVH, material, and intersection diagnostics |
| `--threads N` | Set CPU thread count |

## Next Steps

Material/texture work is anchored on **IridescenceLamp** (Khronos sample:
3 materials, base color + ORM + iridescence-thickness textures,
transmission/ior/volume/iridescence extensions). Full phased plan and bug
status in [nextsteps.md](nextsteps.md).

### Phase 1 — Per-pixel PBR (foundation)
- Fix ORM roughness CPU/GPU parity bug (GPU sRGB-converts a linear texture)
- Per-pixel metallic from ORM B channel, AO from ORM R channel
- Merge plastic/metallic material classes into one PBR branch:
  `diffuse × (1−metallic)`, `F0 = mix(0.04, basecolor, metallic)`

### Phase 2 — KHR_materials_iridescence
- Parse extension (factor, IOR, thickness min/max, thickness texture)
- Load thickness texture; thin-film interference tint on the specular lobe

### Phase 3 — Volume absorption
- Use `KHR_materials_volume` thickness in the glass transmission path

### Longer term
- **True area lights** — softer, more realistic shadows
- **GPU animation** — currently falls back to CPU
- **JSON schema validation** for scene files
- **Web viewer diff panel** — load mesh_stats.json alongside Three.js data for automated comparison
