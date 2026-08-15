# Web-Based Real-Time glTF Viewer — Implementation Spec

**Target model:** DeepSeek-V4-Flash (agent loop, OpenCode)
**Repo:** `~/Dev/ray` (github.com/jalfern/ray)
**New directory:** `web_viewer/` at repo root, separate from the C++ build

---

## 0. Working rules — read before starting

1. **You cannot see rendered output.** Do not state that a model "appears," "renders correctly," or "is now visible." Report only what you can verify: files created, commands exited 0, dev server bound to a port, console errors absent. The human is the sole arbiter of what appeared on screen.
2. **Do not recall API paths from memory — check `node_modules`.** Your training cutoff predates current Three.js. Import paths for `GLTFLoader` and `OrbitControls` have moved between versions (`three/examples/jsm/...` vs `three/addons/...`). After install, `ls node_modules/three/examples/jsm/loaders/` and use what is actually there. Same for Vite config conventions.
3. **Prefer a measurement over a theory.** When something doesn't work, print two numbers and compare them.
4. **Do not port the C++ parser in this phase.** See Design Principle below — this is the point of the project, not an oversight.
5. Work in small verifiable steps. Get a blank Three.js canvas rendering before adding any loader logic.

---

## 1. Goal

A web-based real-time 3D viewer that loads glTF/GLB scenes in a browser with live orbit-camera controls, for debugging geometry, materials, transforms, and camera extraction against the C++ ray tracer.

## 2. Design Principle — deliberate non-reuse

**Do not port or reuse `src/parser/gltf_parser.cc`.** The entire diagnostic value of this viewer is that it is an *independent* implementation. If the C++ parser has a bug (wrong accessor decode, bad transform accumulation), a faithful port of that logic reproduces the bug silently and the viewer teaches us nothing.

Three.js's `GLTFLoader` is independently written and spec-complete. Discrepancies between it and the C++ importer then surface as **visible differences**, not silent agreement on wrong output.

Porting the C++ parser is a *later* phase, at which point it becomes a second implementation to diff against — not the only implementation.

## 3. Immediate motivating bug

The viewer exists to resolve an open bug. Context (do not act on this yet, but scope for it):

- Khronos **IridescenceLamp**, distributed **only as `.glb`** (binary container, no sidecar `.bin`).
- The C++ importer loads 3 meshes. Mesh 1 (`lamp_transmission`, 5,632 tris — the large glass sphere) is present in the BVH and GPU buffer, forced opaque red, unoccluded — and renders **zero pixels**.
- Open question: is the *file* fine and our importer wrong, or is the file not what we think?

**Therefore GLB loading is in scope from day one, and IridescenceLamp is the first entry in the scene dropdown.** A `.gltf`-only viewer does not answer the question it was built for.

---

## 4. Phase 1 — Scaffolding + Three.js GLTFLoader

- `web_viewer/` at repo root, `npm init -y`
- Install with **explicit pins**, then verify actual paths on disk:
  - `three@^0.169.0`
  - `@types/three@^0.169.0`
  - `typescript@^5.6.0`
  - `vite@^5.4.0`
- `index.html` — canvas container + debug overlay panel
- `vite.config.ts` — serve from repo root so `test_scenes/` assets resolve via relative paths
- `src/main.ts` — bootstrap Three.js scene + `OrbitControls`
- Load `.gltf` **and `.glb`** via `GLTFLoader`

**Checkpoint before proceeding:** blank canvas renders, dev server runs, no console errors.

## 5. Phase 1b — Loader abstraction (do this now, not later)

Define the interface up front so the second implementation drops in without refactoring:

```ts
interface ViewerMesh {
  name: string;
  triangles: Float32Array;   // 9 floats per tri (3 verts × xyz)
  normals:   Float32Array;   // same layout
  uvs:       Float32Array;   // 6 floats per tri
  material: {
    type: 'glass' | 'plastic' | 'metallic' | 'emissive';
    color: [number, number, number];
    ior?: number;
    metallic?: number;
    roughness?: number;
  };
}

interface ViewerScene {
  meshes: ViewerMesh[];
  camera: { pos: [number,number,number]; target: [number,number,number]; fovY: number };
}

interface SceneLoader {
  name: string;                                    // 'three.js' | 'ray-importer'
  load(source: File | string): Promise<ViewerScene>;
}
```

Only one implementation exists now (`ThreeGltfLoader`, adapting `GLTFLoader` output into `ViewerScene`). A future `RayImporterLoader` implements the same interface. The UI exposes a **loader dropdown** to switch between them.

**The real payoff is a programmatic diff**, not eyeballing two renders. Build a `diffScenes(a: ViewerScene, b: ViewerScene)` that prints a table: per-mesh **triangle count, bbox min/max, and vertex count**, side by side, with mismatches flagged. Stub it now; it becomes the primary debugging tool once the second loader lands.

## 6. Phase 2 — Renderer

`src/viewer.ts`:
- Load via the active `SceneLoader`, extract scene + cameras
- Materials:
  - metallic/roughness from `pbrMetallicRoughness`
  - transmission + IOR from `KHR_materials_transmission` / `KHR_materials_ior`
  - emissive from `emissiveFactor`
  - map to `MeshStandardMaterial` / `MeshPhysicalMaterial`
- Perspective camera from the glTF `camera` node; sensible default fallback
- `OrbitControls` for pan/zoom/rotate
- Lighting: directional + ambient

### Debug overlays — priority order

**These are the reason the viewer exists. Build them in this order.**

1. **Wireframe toggle** — *highest value.* Reveals triangle topology, degenerate triangles, index-decode errors. This directly addresses the open bug.
2. **Per-mesh stats panel** — name, triangle count, vertex count, bbox min/max, material type. Directly comparable to C++ importer output.
3. **Isolate-mesh mode** — show/hide individual meshes, and force-color a single mesh. Mirrors the red-isolation tests already run in the ray tracer, so results are directly comparable.
4. **`MeshNormalMaterial` toggle** — normal direction / winding verification.
5. **Camera pos/target display** — compare against C++ extraction.
6. **Vertex inspector** — click a triangle, print its vertex positions, normals, UVs.

## 7. Phase 3 — Tooling

- File picker (`<input type="file">`) accepting `.gltf`, `.glb`, and `.bin`
- Scene dropdown, **IridescenceLamp first**:
  - `IridescenceLamp.glb` ← primary target
  - `Box.gltf`
  - `Suzanne.gltf`
  - `Lantern.gltf`
  - `WaterBottle.gltf`
  - `Avocado.gltf`
  - `BoomBox.gltf`
  - `MetalRoughSpheres.gltf`
- `npm run dev` → Vite with hot reload

## 8. Phase 4 — Debugging workflow

1. Load IridescenceLamp.glb
2. Check the per-mesh stats panel: does a mesh named `lamp_transmission` exist with ~5,632 triangles and a bbox ~0.15 units tall?
3. Toggle wireframe → does that mesh have visible triangle topology, or degenerate/collapsed geometry?
4. Isolate that mesh → does it render at all?
5. Compare all of the above against the C++ importer's reported values
6. Toggle normals → verify winding/direction
7. Click triangles → inspect raw vertex data

---

## 9. Out of scope

- **BVH visualization** — would require porting `bvh.cc`. The BVH is already cleared as a suspect (18,866 tris, max_depth 20 vs stack 64). Skip.
- **Custom texture handling** — `GLTFLoader` handles textures automatically; no custom logic needed. Base color factor is sufficient for geometry debugging.
- **WebGPU / WGSL ray tracing** — much bigger lift, defer.
- **Porting the C++ parser** — deliberate, see §2. Later phase.

---

## 10. Definition of done for this pass

- `npm run dev` serves a page that loads `IridescenceLamp.glb`
- Per-mesh stats panel prints name, triangle count, vertex count, bbox for all meshes
- Wireframe toggle works
- Isolate-mesh toggle works
- `SceneLoader` interface in place with one implementation and a stubbed `diffScenes`
- Report to the human: the **stats table output**, verbatim. Do not characterize what the render looks like.
