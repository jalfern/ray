# Sphere not rendering — handoff

## Symptom
Lamp scene: sphere mesh (1152 tris) does not
appear in render. All other meshes render fine.

## VERIFIED — DO NOT RE-CHECK
- BVH depth 20, traversal stack 64. No overflow.
- 532 leaves contain sphere tris, 2032 total leaf
  tris (1152 sphere + 880 other meshes).
- Leaf bboxes correct, within sphere bbox.
- Sphere bbox within root bbox, root is hit.
- CPU and GPU buffer data match.
Re-verifying any of the above is off-limits.

## FIRST TEST
In the shader leaf loop, short-circuit: any tri
whose index is in the sphere's range returns an
immediate hit with flat magenta, skipping the
intersection test.
- Magenta appears -> traversal is fine, the
  ray-triangle test rejects it. Go to suspects.
- Nothing -> counted leaves aren't the ones
  traversal visits. Index indirection bug.

## RANKED SUSPECTS
1. Winding order. Procedural sphere winding may
   be opposite the OBJ meshes, combined with a
   one-sided det test (`det < EPS` rather than
   `fabs(det) < EPS`). Culls every sphere tri.
2. Scale-dependent epsilon. 1152 tris on a small
   sphere = tiny triangles. det scales with edge
   length squared, so a fixed 1e-6..1e-4 kills
   small tris while large floor tris pass. Use a
   relative epsilon.
3. Leaf offset packing overflow. If leaf nodes
   pack first_tri | (count << shift), sphere was
   appended last and its offsets may exceed the
   field width.
4. Rendering black, not missing. Flipped normals
   -> N.L negative -> black on dark bg. Test by
   outputting abs(normal) as color.
