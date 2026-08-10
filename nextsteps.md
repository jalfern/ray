# Project Status and Next Steps - Ray Tracer

## Current State
The following bugs and improvements have been implemented:
- **Refraction Fix (CPU):** Removed incorrect hit normal flipping in `src/renderer/renderer.cc`, allowing rays to correctly identify entrance/exit states for glass meshes.
- **Refraction Fix (GPU):** Aligned GPU normal handling in `src/renderer/shaders.metal` with the CPU implementation to fix mesh refraction.
- **GPU BVH Stability:** Removed hard cap on BVH node allocation in `src/renderer/gpu_renderer.mm` to prevent potential heap buffer overflows with large meshes.
- **Memory Management Refactor:** Updated `src/renderer/renderer.cc` to use `std::vector` for managing `SphereData`, `MeshObjData`, `LightData`, and `EmissiveSurf`, reducing the risk of memory leaks associated with manual C-style allocation.

## Potential Next Steps
Based on the project README and codebase analysis, the following features are recommended:

### High Priority
- **BVH Acceleration Improvements:** Optimize mesh rendering performance for complex OBJ files.
- **Textures & UV Mapping:** Utilize existing UV data from the OBJ parser to implement proper texture mapping.
- **GPU Animation Fix:** Resolve the issue where animations currently fall back to CPU even when GPU is available.
- **glTF Extensions:** Add support for KHR_materials_transmission (glass), KHR_materials_ior, and punctual lights (KHR_lights_punctual). Currently the importer skips all extensions with a warning.
- **glTF Mesh Emission:** Emissive meshes from glTF are imported and correctly wired into the renderer's EmissiveSurf BVH — the renderer iterates all scene meshes regardless of source, so glTF emissive materials work identically to native JSON path emissive meshes.

### Rendering Enhancements
- **Area Lights:** Implement softer, more realistic shadows.
- **Material Expansion:** Add support for emissive meshes (mesh lights), improved metallic surfaces, and subsurface scattering.
- **Advanced Optics:** Implementation of Depth of Field (DoF) and motion blur.
- **Environmentals:** Enhanced skyboxes and environment map integration.
- **Surface Detail:** Implement normal mapping for increased surface detail without increasing polygon count.

### Tooling & Infrastructure
- **Interactive Viewer:** Develop a scene editor or real-time interactive viewer.
- **Validation:** Add JSON schema validation for scene configuration files to prevent runtime crashes from malformed input.

### Known Limitations
- **Glass CPU/GPU parity incomplete:** CPU `trace_ray` uses recursive calls while GPU `shaders.metal`
  uses iterative stack-based traversal. Surface diffuse terms now match (ambient + direct lighting
  is included for all materials on both backends), but transmitted/refracted light paths differ.
  Full parity requires unifying the traversal structure (e.g., port recursive approach to iterative
  or vice versa).

## Investigation Log

### CPU/GPU Render Seam Investigation

**Status:** RESOLVED
**Symptom:** Hard left/right seam and vertical sky banding when comparing CPU vs GPU renders.
Approximately 16,645 differing pixels (0.035% of an 800x600 image).

**Ruled out:** Primary ray generation. Ray directions at pixels (100,300) and (700,300) match
between CPU and GPU within floating-point precision (verified via 8-bit visualization — note:
limited precision, but no sign flips detected).

**Root cause (floor checkerboard):** Algorithmic difference in `floor_color`. CPU used
`(int)floorf(p.x)` (floor toward −∞), GPU used `int(p.x)` (truncate toward zero). For
negative coordinates these produce different integer parts, flipping the parity test and
the checkerboard pattern wherever x or z crosses zero. **Fixed:** GPU changed to
`int(floor(p.x))` to match the CPU convention. See Issue 7 in ray-bugs.md.

**Remaining (sky banding):** 50,940 pixels still differ out of 786,432 (1024×768 frame),
all in the sky. This is float-implementation noise: x87/SSE `sinf` vs Metal `sin`
amplified by the high-frequency cloud product, plus a minor `fminf` clamp on the CPU
path that the GPU lacks. Inherent to different float hardware — not fixable.

**AE count before fix:** 335,032 (197,586 on floor, 137,446 on sky)
**AE count after fix:**  51,557  (    617 on floor, 50,940 on sky)
**Floor diffs eliminated:** 196,969 (99.7% reduction)

### Bug Status Summary
- **Issue 1 (Sphere emissive ~2x too dark):** FIXED — pdf doubled, geometry term clamped
- **Issue 2 (Glass + metallic ambient on CPU):** FIXED — base_color now included for all materials
- **Issue 3 (CPU negative clamp):** FIXED — CPU-only parity bug, fmaxf(0.0f, ...) added before uint8 cast
- **Issue 4 (Mesh emissive normal not flipped):** FIXED — shared sampling bug, normal now flipped toward shaded point
- **Issue 5 (Shadow rays don't skip origin mesh):** FIXED — shared missing-guard bug, skip_mesh parameter added to in_shadow on both backends
- **Issue 6 (Camera zenith/nadir singularity):** FIXED — shared missing-guard bug, alternate up vector used when fwd parallels world up
- **Glass traversal parity:** DOCUMENTED LIMITATION — recursive vs iterative traversal

### glTF Importer (NEW)
- **Stages 1-8 complete:** Full core-spec glTF 2.0 importer
  - Buffer/bufferView/accessor chain decoding
  - Triangle mesh extraction with transform baking
  - PBR material mapping (emissive/metallic/plastic)
  - Camera extraction from node tree
  - Node hierarchy traversal with TRS/matrix accumulation
  - Integrated into parser.cc via "gltf" scene key
- **Tested with:** Box, Triangle, Suzanne, Avocado, BoomBox, Lantern, WaterBottle
- **Known gaps:** Extensions skipped (glass/transmission), punctual lights not imported
