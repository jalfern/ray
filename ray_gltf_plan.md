# glTF 2.0 Importer — Implementation Plan

## Design principle

The importer is a pure translation layer. No glTF-specific types escape into
`scene.h`, `mesh.h`, `renderer.cc`, or `shaders.metal`. It produces the same
`Scene` / `MeshObj` / `TriGpu` structs that the OBJ path already fills —
the renderer never knows the data came from glTF.

## Status: ALL STAGES COMPLETE

The importer is fully integrated and tested. Renders Suzanne, Box, Avocado,
BoomBox, Lantern, and WaterBottle from glTF sample models. See nextsteps.md
for known limitations and next steps.

## Files

| File | Role |
|------|------|
| `src/parser/gltf_parser.h` | Public API: `int load_gltf(const char* path, GltfScene* out)` |
| `src/parser/gltf_parser.cc` | All JSON walking, buffer decoding, node traversal, material mapping |
| `src/parser/parser.cc` | One new branch: if JSON has `"gltf"` key, call `load_gltf()` instead of the usual meshes/lights/floor pipeline |

No changes to `scene.h`, `mesh.h`, `renderer.cc`, or any GPU shader code.

## GltfScene output struct

```c
typedef struct {
    // Meshes
    MeshObj* meshes;
    int num_meshes;
    // Camera
    Vec3 camera_pos;
    Vec3 camera_target;
    float aperture;   // default 0
    float focus_dist; // default from camera-to-target
} GltfScene;
```

`MeshObj` already carries `tris`, `num_tris`, `pos`, `scale`, `color`,
`material`, `reflectivity`, `ior`. The importer fills these directly.

## Stages (all complete)

### 1. File scaffolding
- Create `gltf_parser.h` / `gltf_parser.cc`
- Add `src/parser/gltf_parser.o` to the Makefile SOURCES list
- Stub `load_gltf()` that returns 0 (ok) with an empty scene

### 2. JSON token helpers
- glTF is plain JSON, no extensions
- Borrow the same `skip_ws` / `parse_string` / `parse_float` / `parse_int`
  pattern from `parser.cc` (or share via a small header)
- Add key-aware walking: given a JSON object, find a key and return its
  value token, handling nested arrays/objects, strings, and numbers

### 3. Buffer → bufferView → accessor decode
The core data-path function. For each accessor:

1. Resolve the chain: `accessor → bufferView → buffer`
2. Compute element byte size from `componentType` and `type`:
   - FLOAT (5126) = 4 bytes, UNSIGNED_SHORT (5123) = 2, UNSIGNED_INT (5125) = 4
   - "SCALAR" = 1 component, "VEC2" = 2, "VEC3" = 3
3. Compute stride: `stride = max(elementSize, bufferView.byteStride)`
   (0 stride = tightly packed)
4. Iterate `count` elements, read each at
   `bufferView.byteOffset + accessor.byteOffset + i * stride`
5. Convert to float for positions/normals/texcoords, or int for indices
6. Store in a flat `float[]` or `int[]` owned by the function

Output: a simple struct `{float* data, int count, int components}` per
accessor. The function handles component type conversion transparently.

### 4. Mesh → TriGpu extraction
For each `mesh` in the glTF:
- Iterate `primitives[]`, skip anything where `mode != 4` (TRIANGLES)
- For each primitive, decode:
  - `"POSITION"` accessor → 3-component float positions
  - `"NORMAL"` accessor → 3-component float normals
  - `"TEXCOORD_0"` accessor → 2-component float UVs (optional)
  - `"indices"` accessor → triangle indices
- Build `TriGpu` array: for each index triplet (i0,i1,i2), copy
  positions and normals into a `TriGpu`
- No index buffer is kept; this is a flat triangle array, same as OBJ
  path.

### 5. Node tree walk + transform accumulation
- Parse `scenes[0].nodes` to find the root node list
- Recursively walk each node:
  - Accumulate transform from `translation` (Vec3), `rotation` (quat),
    `scale` (Vec3) → 4×4 matrix
  - If node has a `mesh` field, bake the transform into its vertex
    positions (rotate normals by the 3×3 rotation part)
  - Append transformed triangles to the output array
- In v1, no instancing optimization — flatten everything. The OBJ path
  bakes transforms the same way.

### 6. Material mapping
For each `material` in `materials[]`:
- Read `pbrMetallicRoughness.baseColorFactor` (default [1,1,1,1])
- Read `pbrMetallicRoughness.metallicFactor` (default 1.0)
- Read `emissiveFactor` (default [0,0,0])
- Classification:
  - `emissiveFactor` has any component > 0 → `MAT_EMISSIVE`,
    `color = emissiveFactor`
  - `metallicFactor > 0.5` → `MAT_METALLIC`, `color = baseColorFactor`
  - else → `MAT_PLASTIC`, `color = baseColorFactor`
- `reflectivity` = metallicFactor (rough approximation)
- `ior` = 1.5 (hardcoded — no IOR in core spec)
- If `extensionsRequired` is non-empty → log warning, classify as
  `MAT_PLASTIC`

### 7. Camera
- Find the first `node` with a non-null `camera` field
- Read `camera.perspective.yfov` for vertical FOV
- Derive `camera_pos` and `camera_target` from the camera node's
  world-space transform:
  - Position = transform translation
  - Target = position - forward vector (extracted from rotation matrix)
- `aperture` = 0 (no DOF), `focus_dist` = distance to target

### 8. Integration into parser.cc
- Add a `"gltf"` key to the scene JSON schema
- In `parse_scene`, when `"gltf"` is present:
  - Call `load_gltf()` to get `GltfScene`
  - Copy `GltfScene.meshes` into `Scene.meshes`
  - Copy camera into `Scene.camera_pos` / `Scene.camera_target`
  - Only override camera if glTF actually has one (fix: skip zero camera)
  - The existing spheres/lights/floor keys in the JSON are additive

### 9. Testing
- Find or create a minimal core-spec glTF scene (triangles, one
  material, one camera, no extensions)
- Render and visually verify
- Test with `extensionsRequired` to confirm graceful fallback
- Test with multi-node hierarchy to confirm transform accumulation