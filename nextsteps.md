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

### Rendering Enhancements
- **Area Lights:** Implement softer, more realistic shadows.
- **Material Expansion:** Add support for emissive meshes (mesh lights), improved metallic surfaces, and subsurface scattering.
- **Advanced Optics:** Implementation of Depth of Field (DoF) and motion blur.
- **Environmentals:** Enhanced skyboxes and environment map integration.
- **Surface Detail:** Implement normal mapping for increased surface detail without increasing polygon count.

### Tooling & Infrastructure
- **Interactive Viewer:** Develop a scene editor or real-time interactive viewer.
- **Validation:** Add JSON schema validation for scene configuration files to prevent runtime crashes from malformed input.
