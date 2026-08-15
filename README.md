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
| KHR_materials_volume (thickness) | Parsed |
| Punctual lights (KHR_lights_punctual) | — |
| KHR_materials_iridescence | — |
| glTF texture/color from baseColorTexture | — |

## Known Limitations

- **Glass traversal parity:** CPU uses recursive tracing, GPU uses iterative stack — transmitted light paths can diverge (both backends produce valid images, but refracted paths may differ)
- **Scale-relative ray-triangle test:** The old absolute EPS=1e-4 threshold rejected small triangles regardless of ray direction. Now uses `|det| < 1e-7 * mean(|e1|², |e2|²)` — verified against IridescenceLamp's glass sphere (5,632 tris, ~0.01-unit edges).

## Debug Flags

| Flag | Effect |
|------|--------|
| `--cpu` | Force CPU rendering path |
| `--mesh-stats` | Print geometry, BVH, material, and intersection diagnostics |
| `--threads N` | Set CPU thread count |

## Next Steps

### Short term
- **glTF baseColorTexture sampling** — all IridescenceLamp materials reference textures but the parser never feeds them to the shader
- **KHR_materials_iridescence** — parsed but not applied; affects all three lamp materials
- **Normal mapping** for increased surface detail

### Longer term
- **True area lights** — softer, more realistic shadows
- **GPU animation** — currently falls back to CPU
- **JSON schema validation** for scene files
- **Web viewer diff panel** — load mesh_stats.json alongside Three.js data for automated comparison
