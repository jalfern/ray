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

## Architecture

- **parser/** - JSON scene parsing → `Scene` struct
- **renderer/** - Ray tracing core → `Image` struct
- **output/** - PPM/PNG writers
- **vector/** - 3D vector math operations

## Next Steps

- **BVH acceleration** — mesh rendering will slow down with more complex OBJs
- **Textures/UV mapping** — OBJ parser already reads UVs but they're unused
- **Area lights** — softer, more realistic shadows
- **GPU animation** — currently animation falls back to CPU even with GPU available
- **More materials** — emissive, metallic, subsurface scattering
- **Depth of field / motion blur**
- **Skybox / environment maps**
- **Normal mapping**
- **Emissive objects / mesh lights**
- **Scene editor / interactive viewer**
- **Refraction fix for glass meshes** — entering/exiting IOR regions only handled for spheres
- **JSON schema validation**
