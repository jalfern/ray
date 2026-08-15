# Debug Session — 2026-08-13

## Scene: IridescenceLamp (test_scenes/scene_lamp.json)
- glTF file: `test_scenes/IridescenceLamp/IridescenceLamp.gltf`
- 3 root nodes, no parent-child hierarchy, all transforms = identity
- 3 meshes, each with 1 primitive (no dropped primitives)
- 12 accessors total (4 per mesh: indices, positions, normals, UVs)
- 1 buffer, 3 bufferViews

## Mesh Identity (CPU index order = node traversal order: 2, 1, 0)

| CPU idx | glTF node | glTF mesh | Name | Tris | World bbox | Identity |
|---------|-----------|-----------|------|------|------------|----------|
| 0 | node 2 | mesh 2 | `lamp_iridescence` | 1152 | y=0.243–0.472, w=0.292 | **SHADE** |
| 1 | node 1 | mesh 1 | `lamp_transmission` | 5632 | y=0.051–0.201, w=0.194 | **GLASS SPHERE (small)** |
| 2 | node 0 | mesh 0 | `lamp` | 12082 | y=0.000–0.476, w=0.300 | **FULL LAMP (all parts)** |

## Key Findings

### 1. All geometry loads — nothing is dropped
- 3 meshes × 1 primitive each = all primitives accounted for
- 18866 total tris = 1152 + 5632 + 12082 ✓
- 12 accessors, all decoded correctly
- Indices use UNSIGNED_SHORT (5123), correctly handled

### 2. No transform bug — all node transforms are identity
- glTF nodes have NO translation/rotation/scale/matrix fields
- Parser correctly defaults to identity transforms
- World-space bboxes match raw accessor bboxes exactly

### 3. BVH is correct
- 11865 nodes, depth 20, 532 sphere-containing leaves, 2032 leaf tris
- CPU and GPU buffer data match
- bbox_hit function is correct slab method

### 4. Mesh 1 (lamp_transmission, 5632 tris) produces ZERO visible pixels
- Triangles are non-degenerate (0/5632 degenerate, area2 ≈ 6e-11)
- Loaded into GPU buffer, in BVH, forced opaque red in shader
- Still produces no red pixels in render
- **Root cause unresolved**: likely either BVH traversal culls its tiny leaf bboxes, or hit_tri rejects its small triangles (scale-dependent epsilon), or the geometry is so fine it's sub-pixel

### 5. The "big glass sphere" is absent from all meshes
- Mesh 2 (full lamp, 12082 tris) forced red shows only shade + stem + base — no sphere
- Mesh 1 (transmission, 5632 tris) forced red shows nothing
- The large spherical shell visible in reference images is not present in the geometry
- The sphere in mesh 1 is only 0.15 units tall (y=0.051–0.201) — a small inner globe, not the outer sphere

### 6. Suspect: scale-dependent epsilon in hit_tri
- Mesh 1 triangles have area2 ≈ 6e-11, edge lengths ~0.003
- `det` in Möller–Trumbore scales with edge length squared
- The fixed epsilon `EPS` (likely 1e-6 or 1e-7) kills small triangles while large ones pass
- This would explain why mesh 2's large triangles render but mesh 1's tiny ones don't
- **Next diagnostic**: print the EPS value used in shaders.metal, then try a relative epsilon (e.g., `fabs(det) < 1e-12 * max_edge_len_sq`)

## Open Questions
1. What is the exact EPS value in shaders.metal?
2. Does replacing the absolute epsilon with a relative epsilon fix mesh 1's visibility?
3. If mesh 1's triangles are 0.003-unit edges at y=0.05–0.20, what is the outer sphere's geometry — is it only in mesh 0 (the 12082-tri "lamp") and we just can't see it because of material/transparency?
4. Verify by forcing ALL tris to pass hit_tri (not just mesh 1) and see if the sphere appears