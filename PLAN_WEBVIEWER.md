# Web-Based Real-Time glTF Viewer — Plan

## Goal
Create a web-based real-time 3D viewer that loads glTF scenes in a browser,
with live orbit-camera controls for debugging geometry, materials, and camera
extraction.

## Design Principle
**Deliberate non-reuse of the C++ parser.** The whole point of an independent
viewer is that it serves as a cross-check — if the C++ glTF parser has a bug
(e.g. wrong accessor decoding, bad transform accumulation), a faithful port
of that same logic will reproduce the bug silently. Using Three.js's
independently-written `GLTFLoader` means discrepancies surface as visible
differences, not silent agreement on wrong output.

## Phase 1: Project Scaffolding + Three.js GLTFLoader
- Create `web_viewer/` at repo root with `npm init -y`
- Install: `three`, `typescript`, `vite`, `@types/three`
- `index.html` — canvas container + debug overlay panel
- `vite.config.ts` — serve from repo root so `.bin` files load via relative paths
- `src/main.ts` — bootstrap Three.js scene with `OrbitControls`
- Use `THREE.GLTFLoader` to load any `.gltf` file (handles JSON + binary + images)
- No custom parser port — Three.js is the independent implementation

## Phase 2: Three.js Real-Time Renderer
- `src/viewer.ts`:
  - Load glTF via `GLTFLoader`, extract `scene` and `cameras`
  - Render all meshes with `MeshStandardMaterial` / `MeshPhysicalMaterial`
    - Metallic/roughness from `pbrMetallicRoughness`
    - Transmission + IOR from `KHR_materials_transmission` / `KHR_materials_ior`
    - Emissive from `emissiveFactor`
  - Perspective camera from glTF `camera` node, fallback to default if absent
  - `OrbitControls` for pan/zoom/rotate
  - Basic lighting: directional light + ambient light
  - **Debug overlays:**
    - Wireframe toggle (geometry topology debugging)
    - `MeshNormalMaterial` toggle (normal direction / winding debugging)
    - Triangle count + mesh count display
    - Camera position/target display (compare against ray tracer)
    - Vertex inspector — click a triangle to see its vertex positions, normals, UVs

## Phase 3: Tooling & Integration
- File picker (`<input type="file">`) for drag-and-drop `.gltf` + `.bin`
- Dropdown to select from existing test scenes:
  - `test_scenes/Box.gltf`
  - `test_scenes/Suzanne.gltf`
  - `test_scenes/Lantern.gltf`
  - `test_scenes/WaterBottle.gltf`
  - `test_scenes/Avocado.gltf`
  - `test_scenes/BoomBox.gltf`
  - `test_scenes/MetalRoughSpheres.gltf`
- `npm run dev` → Vite dev server with hot reload

## Phase 4: Debugging Workflow
1. Load a glTF scene in the browser
2. Toggle wireframe → verify triangle topology matches what the ray tracer sees
3. Toggle `MeshNormalMaterial` → verify normal directions/winding
4. Compare camera position/target displayed in the viewer vs. what the C++ parser extracts
5. Click individual triangles → inspect vertex/normal/UV data for decode bugs
6. Compare material classification (glass/metallic/plastic/emissive) between viewer and ray tracer

## Dependencies to install
- `three` (r158+) — 3D engine with GLTFLoader
- `typescript` — type safety
- `vite` — dev server + bundler
- `@types/three` — TypeScript types

## Out of scope (future features)
- BVH visualization (would require porting bvh.cc — skip for now)
- Image textures (Three.js GLTFLoader handles these automatically, but we don't need custom logic)
- GPU ray tracing in the browser (WebGPU/WGSL — much bigger lift)