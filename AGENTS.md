# Ray Tracer - Agent Instructions

## Build and Run

```bash
make              # Build ray2 binary
make run          # Build + render scenes/scene.json
make test         # Render all test scenes (torus, ico, vase, demo, emissive, complex)
make clean        # Remove build/, ray2, build/tools/
make models       # Build tools/ generators + regenerate models/*.obj
```

**Compiler**: `g++` with `-Wall -Wextra -O2 -I./include -std=c++11`. Link with `-lm -lz -framework Metal -framework Foundation`.

**Crucial**: `src/renderer/gpu_renderer.mm` is Objective-C++ and must be compiled with `-ObjC++ -fobjc-arc`. The Makefile handles this separately — if you add `.mm` files, follow that rule pattern.

**macOS only**: GPU rendering requires Metal + Foundation frameworks. CPU-only builds are possible but the renderers and entry point expect Metal symbols; removing the Metal dependency end-to-end is non-trivial.

## Usage

```bash
./ray2 scenes/scene.json                 # PPM to stdout, PNG to file from scene config
./ray2 --cpu scenes/scene.json          # Force CPU rendering
./ray2 --threads 4 scenes/scene.json    # Pin CPU thread count
```

Animation scenes (JSON has `"animation"` key) write frames to `frames/` then invoke `ffmpeg` to produce video. Requires `ffmpeg` on PATH.

## Source Layout

```
include/          Shared headers — types.h, vector.h, scene.h, mesh.h, bvh.h, parser.h
src/main.cc       Entry point; arg parsing, CPU/GPU dispatch, animation loop, ffmpeg call
src/vector/       3D vector math
src/parser/       JSON scene parser + OBJ mesh parser
src/renderer/     CPU renderer (renderer.cc), BVH (bvh.cc), Metal GPU (gpu_renderer.mm, shaders.metal)
src/shading/      Floor patterns and material helpers
src/output/       PPM and PNG writers
src/denoiser/     Image denoising
src/envmap/       Environment map / skybox support
tools/            Mesh generators (gen_torus, gen_ico, gen_vase) — build with `make models`
scenes/           JSON scene files (see scenes/scene.json for schema reference)
models/           OBJ mesh files (regenerated via `make models`)
```

## Scene JSON Schema

Key fields: `camera` (pos, target), `spheres`, `meshes`, `lights`, `floor`, `width`, `height`, `output`. Add `animation` block (duration, fps, orbit) for animated renders. Mesh `file` paths are relative to the scene file. Valid materials: `glass`, `plastic`, `emissive`, `metallic`, `subsurface`.

## Gotchas

- GPU animation falls back to CPU per frame — the GPU path does not support animation yet.
- Refraction for glass meshes requires correct normal handling at both entry and exit; the CPU and GPU implementations must stay aligned (`renderer.cc` vs `shaders.metal`).
- `make test` renders 6 scenes silently (stdout → `/dev/null`); output PNGs go to `images/`.
- Changes to `.h` files in `src/` (e.g., `renderer/renderer.h`) will trigger recompilation of dependent `.o` files automatically via the pattern rule.
