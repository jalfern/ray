# Ray Tracer (Refactored)

Componentized ray tracer with clean separation of concerns.

## Build

```bash
make          # Build
make clean    # Remove build artifacts
make run      # Build and run with default scene.json
```

## Usage

```bash
./ray2 scene.json      # Output PPM to stdout, PNG to file specified in scene.json
./ray2 custom.json > image.ppm  # Override output to stdout
```

## Architecture

- **parser/** - JSON scene parsing → `Scene` struct
- **renderer/** - Ray tracing core → `Image` struct
- **output/** - PPM/PNG writers
- **vector/** - 3D vector math operations

## Next Steps

- Extract shading logic into separate module
- Add acceleration structures (BVH)
- Port parser/output to Python (keep C renderer via CFFI)
