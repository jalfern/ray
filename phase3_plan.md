# Phase 3 — Normal maps + standalone AO + MASK

Status: planned, not started. Written 2026-08-29.

This plan supersedes the Phase 3 stub in `iridescent_dish_nextsteps.md`.
Every file path and struct name below was verified against the tree at
`HEAD` — do not re-derive them, but do report if anything has moved.

---

## 0. Prerequisite (DONE — do not redo)

`ao_tex_index` is plumbed end to end, landed in **74acd37**:

- `MeshObj` (`include/scene.h`) — last field, fills the tail padding at
  offset 148, `sizeof` stays 152
- `MeshMatGpu` (`src/renderer/gpu_renderer.mm`) — offset 108,
  `sizeof` 112, `static_assert` updated
- `MeshMat` (`src/renderer/shaders.metal`) — mirrors `MeshMatGpu`
- Upload loop in `gpu_renderer.mm` copies the field
- `gltf_parser.cc` parses `occlusionTexture`, `-1` sentinel when absent

No shading term reads it yet. Phase 3 is where it gets consumed.

---

## 1. Facts established during planning

These were found by reading the tree and the test asset. They correct
several assumptions in the earlier draft.

**Three structs in the mesh chain, not two.**
`scene.h`'s `MeshObj` → `types.h`'s `MeshObjData` (what the CPU renderer
actually consumes) → `mesh.h`'s `TriGpu`. New material fields have to
cross all of it.

**CPU shading lives in `src/renderer/renderer.cc`**, not
`src/shading/shading.cc`. `shading.cc` holds texture helpers and material
property lookups only.

**`TriGpu` is currently 100 bytes** (`include/mesh.h`):
`v0/v1/v2` 36, `n0/n1/n2` 36, `t0/t1/t2` 24, `mesh_idx` 4.

**54 sites construct or fill `TriGpu`** — `obj_parser.cc`, `parser.cc`,
`tools/gen_*.c`, and others. Any layout change touches all of them.

**Metal is compiled with fast math ON.** The library is created with
`newLibraryWithSource:options:nil`, and a nil `MTLCompileOptions` means
`fastMathEnabled` defaults to YES. The Makefile has no `-ffast-math`, so
the CPU side does not.

**`sample_texture` applies `srgb_to_linear`.** Correct for base colour,
wrong for normal maps and AO — both are data, not colour. They need a
linear sampling path on both backends.

**The test asset has no `TANGENT` attribute.** So the glTF-supplied
tangent path has nothing to read here; generation is the live path.

**The asset's `occlusionTexture` points at the ORM texture.** e.g. mesh 1
(olives) has `occlusionTexture` index 2 = `olives_orm.png`. Per glTF, the
R channel of whatever texture sits in the `occlusionTexture` slot is AO —
regardless of whether that same image also serves as ORM elsewhere. So
the branch is not "standalone vs ORM", it is "is `ao_tex_index` set;
sample its R channel; else fall back to `orm_tex_index`'s R channel".

---

## 2. Tangents

**Storage.** Per-vertex, mirroring the existing `n0/n1/n2` pattern:
`float tan0[4], tan1[4], tan2[4]` on `TriGpu`. +48 bytes, taking the
struct from 100 to 148. Per-triangle storage (+16) would be cheaper but
produces faceted shading across smooth surfaces and throws away the
smoothing MikkTSpace exists to provide.

The 4th component carries bitangent handedness. It is not optional —
dropping it flips normals on mirrored UV islands.

**Generation: MikkTSpace, vendored.** Take the reference implementation
(`mikktspace.c` / `mikktspace.h`, public domain, two files) into the tree
rather than writing one. Wire the four callbacks to our mesh data.

The reason for MikkTSpace specifically: normal maps are baked in a
particular tangent space, and MikkTSpace is the near-universal DCC
default. A different generator produces subtly wrong lighting everywhere
with no obvious symptom. Matching the baker matters more than the
algorithm's own merits.

**Where it runs: pre-bake, on the original primitive.** Feed it
`pd->positions`, `pd->indices`, `pd->texcoords`, `pd->normals` before the
world-space bake. Two reasons:

- Normal maps are authored against the mesh's own object/UV space, which
  is what the baking tool saw.
- After the world-space bake the mesh is a triangle soup with no vertex
  sharing, so MikkTSpace would emit per-triangle tangents with no
  smoothing — defeating the point.

Then transform the resulting tangents into world space alongside the
normals and re-orthogonalize against the transformed normal
(Gram-Schmidt). This handles non-uniform scale adequately; do not
special-case node transforms.

**Priority.** glTF `TANGENT` attribute when present; MikkTSpace when
absent. This asset takes the second path.

---

## 3. Normal mapping

**CPU** (`src/renderer/renderer.cc`): build the TBN from the interpolated
tangent and normal, sample `normalTexture` through a **linear** path
(no `srgb_to_linear`), unpack `[0,1]` → `[-1,1]`, apply
`normalTexture.scale`.

**GPU** (`src/renderer/shaders.metal`): mirror the CPU logic using the
tangent and normal from the expanded `TriGpu`.

**Scale is on the critical path.** In the test asset, mesh 0
(`glassdish_irid`) and mesh 2 (`goldLeaf`) have scale 1, but mesh 1
(`glassCover`) has **scale 2**. A bug in scale handling is invisible on
two of three meshes and wrong on the third — exactly the kind of thing a
casual visual check passes.

---

## 4. AO

Sample the R channel of `ao_tex_index`'s texture when set, else fall back
to `orm_tex_index`'s R channel. **Linear sampling**, not sRGB.

Apply to both the ambient term and the per-light diffuse term. Specular
stays un-AO'd, matching the existing design decision.

---

## 5. MASK (alpha cutoff)

**Design: test inside the leaf-node triangle loop.** After `hit_tri`, if
the triangle's material is MASK and the sampled alpha at `(u,v)` is below
the cutoff, `continue` — treat that triangle as simply not hit. No
re-search, no `skip_tri` parameter, no outer bounce-loop change.

- GPU: the BVH walk is inline in `trace_ray`, which already has
  `scene_tex` and `mats` access.
- CPU: `hit_mesh_bvh` (`renderer.cc:163`) is standalone and needs the
  texture data passed in. The BVH is per-mesh, so every triangle in a
  given walk shares one material — pass that mesh's `tex_index`,
  `alpha_mode`, `alpha_cutoff` and the image, not the whole bundle.

**Open problem — MASK breaks byte-identity.** The CPU does hand-rolled
bilinear on RGBA8; the GPU does hardware bilinear on RGBA8Unorm. Those
differ in the last bit, which is the existing 0.23% cross-backend floor.

For colour, a last-bit difference is a last-bit difference. For an alpha
test at cutoff 0.5, a pixel whose alpha lands near 127/128 can flip from
hit to miss between backends. The ray then goes somewhere entirely
different, and the pixel differs by hundreds — not by 1/255. No ε bound
on channel error catches that, because the divergence is in *which
surface was hit*, not in the shading.

Three ways out:

1. **Hand-rolled bilinear for the alpha channel on the GPU too.** Matches
   the CPU exactly, preserves byte-identity, and is only needed for the
   alpha test rather than general sampling. **Recommended.**
2. Nearest-neighbour alpha sampling on both backends. Removes the
   divergence entirely; costs aliased mask edges.
3. Accept it and widen the gate — record "≤N pixels may differ
   arbitrarily where alpha ∈ [0.49, 0.51]" as part of the scene's
   signature.

Decide this before writing the MASK code; the gate definition depends on
it.

---

## 6. Fast math

Do **not** disable fast math globally — it is currently on for Metal and
the whole shading path benefits.

Write the TBN math stable enough not to need it. If the error bound is
exceeded, escalate to a **localized** non-fast-math block for the
normal-mapping logic only. The mechanism is `#pragma METAL fp
math_mode(safe)` around the block, or a separately compiled library with
`-fno-fast-math`. It is **not** `[[attribute(constant)]]` — that is a
vertex attribute qualifier and unrelated.

---

## 7. Verification

Per the two-tier standard in `AGENTS.md`:

- **Commit delta** (before vs after, same backend): 0%, byte-identical.
  Non-zero is a bug in the commit.
- **Cross-backend** (CPU vs GPU): error-bounded, never zero. Gate is "no
  worse than the recorded baseline signature." Recorded floor for
  iri-dish-256 is **137 px / 0.23% / sum 147 / max 1/255**.
- **TBN float paths specifically**: L∞ bound of **ε = 2/255** per channel.

**Layout audit before any shader code.** Report `sizeof` and field
offsets for `TriGpu`, `MeshObj`, `MeshObjData`, `MeshMatGpu` and
`MeshMat` in both C++ and MSL. If a change causes alignment padding,
update both definitions in a single synchronized commit. Use `int32_t`
explicitly, not `int`.

**Regression scene.** Also render a scene with no normal maps and confirm
the sentinel path leaves it byte-identical to the pre-change baseline.

---

## 8. Sequencing

One commit per step. Verify the commit-delta gate before moving on.

1. **`TriGpu` expansion + tangent generation.** All 54 construction sites
   updated, MikkTSpace vendored and wired, tangents populated but **not
   read by any shading term**. Gate: 0 px commit delta on all six test
   scenes. A missed constructor shows up here as garbage in one scene,
   not as a subtle shading error later.
2. **Normal mapping shading.** CPU and GPU. Gate: commit delta is
   expected to be non-zero (this changes the image); cross-backend must
   stay within ε = 2/255 of the recorded floor.
3. **AO shading.** Same gates.
4. **MASK.** Only after the alpha-sampling decision in §5 is made.

Do not combine steps 1 and 2. The whole point of the split is that a
layout bug and a shading bug look different.
