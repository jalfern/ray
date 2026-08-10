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
./ray2 scenes/scene.json          # Output PPM to stdout, PNG to file specified in scene.json
./ray2 scenes/custom.json > image.ppm  # Override output to stdout
./ray2 --cpu scenes/scene.json    # Force CPU rendering
```

## Project Layout

```
├── scenes/              # Scene JSON files
├── models/              # OBJ mesh files
├── images/              # Rendered output images
├── videos/              # Rendered animation videos
├── src/
│   ├── main.cc          # Entry point, animation loop
│   ├── parser/          # JSON scene + OBJ parsing
│   ├── renderer/        # Ray tracing core (CPU + Metal GPU)
│   ├── shading/         # Material properties, floor pattern
│   ├── vector/          # 3D vector math
│   └── output/          # PPM/PNG writers
├── include/             # Shared type definitions
└── tools/               # Mesh generators (torus, ico sphere, vase)
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
| Punctual lights from glTF (KHR_lights_punctual) | — |
| Transmission / IOR from glTF (KHR_materials_transmission/ior) | — |

## Known Limitations

- **Glass traversal parity:** CPU uses recursive tracing, GPU uses iterative stack — transmitted light paths can diverge (both backends produce valid images, but refracted paths may differ)

## Next Steps

### Short term
- **glTF extensions:** KHR_materials_transmission (glass), KHR_materials_ior, KHR_lights_punctual
- **Normal mapping** for increased surface detail

### Longer term
- **True area lights** — softer, more realistic shadows
- **GPU animation** — currently falls back to CPU
- **JSON schema validation** for scene files
- **Interactive viewer**
