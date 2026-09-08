# Phase 3 — Normal maps + standalone AO + MASK

Status: in progress. Staged plan rewritten 2026-09-06 against HEAD
`0b62cb9` (supersedes the 2026-08-29 draft, which is retained in git
history). Every file path, struct size, and line citation below was
re-verified against the tree at `0b62cb9` — report if anything has moved.

The feature scope is unchanged from `iridescent_dish_nextsteps.md` Phase 3:
parse `normalTexture` (+ scale) and standalone `occlusionTexture` into the
material, apply a tangent-space normal perturbation to the unified PBR
shading on both backends, consume the already-plumbed `ao_tex_index`, and
implement `alphaMode: MASK`.

---

## 0. Prerequisites and corrections (verified 2026-09-06)

**`ao_tex_index` is NOT plumbed end-to-end.** The 2026-08-29 draft claimed
end-to-end plumbing (74acd37). True for the GPU half and the parser:
`MeshObj` (`include/scene.h:58`, offset 148, `sizeof` 152), `MeshMatGpu`
(`src/renderer/gpu_renderer.mm:178`, offset 108, `sizeof` 112,
`static_assert` `:186`), `MeshMat` (`src/renderer/shaders.metal:456`),
upload copy `gpu_renderer.mm:384`, parse `gltf_parser.cc:1637-1644`.
**False for the CPU half**: `MeshObjData` (`include/types.h:38-66`) has no
`ao_tex_index`, and `setup_context` (`renderer.cc:1057-1078`) does not copy
it — the CPU renderer cannot see it. Stage 2 adds it.

**The GPU texture bundle already contains every image.**
`gltf_parser.cc:1957-1965` copies *all decoded images* into
`out->textures[]` regardless of which material slot references them, and
`gpu_renderer.mm:558-573` uploads every one of them into the 64-slot
`TexBundle`. Normal-map images therefore need **no bundle changes** — only
the per-material index plumbing.

**Textures load RGBA8.** `stbi_load(full, &w, &h, &ch, 4)`
(`gltf_parser.cc:1440`) — the alpha channel needed by MASK is already in
`ImageTexture.data` on both backends (GPU textures are
`MTLPixelFormatRGBA8Unorm`, `gpu_renderer.mm:562`).

**Not parsed anywhere in `src/`** (verified by grep): `normalTexture`,
`alphaMode`, `alphaCutoff`, `TANGENT`. The `GltfPrimitiveRef` /
`GltfPrimitiveData` pair (`include/gltf_parser_internal.h:34-55`) carries
pos/norm/tex/idx accessors only.

**Struct sizes at HEAD** (asserted where noted):

| Struct | File | Size | Note |
|---|---|---|---|
| `TriGpu` | `include/mesh.h:4` / `shaders.metal:98` | 100 B | v 36 + n 36 + t 24 + mesh_idx 4 |
| `MeshObj` | `include/scene.h:30` | 152 B | `ao_tex_index` last, offset 148 |
| `MeshObjData` | `include/types.h:38` | — | CPU-side copy; **missing** `ao_tex_index` |
| `MeshMatGpu`/`MeshMat` | `gpu_renderer.mm:154` / `shaders.metal:432` | 112 B | `static_assert` `:186` / `:84` |
| `SceneGpu` | `gpu_renderer.mm:125` / `shaders.metal:58` | 140 B | `static_assert` `:184` / `:84` |

**Build gotchas** (unchanged): the Makefile has no header dependency
tracking — any struct change requires `make clean && make`. The Makefile
compiles an explicit list of `.cc` files and has **no `.c` rule** —
vendoring `mikktspace.c` needs a new rule (or compile it as C++). Metal is
compiled at runtime with `newLibraryWithSource` and nil options, i.e.
fast-math ON; the CPU side has no `-ffast-math`.

**Parity state at HEAD**: the no-arg gate (dish256 + envtest) PASSES.
Current baselines (`tools/parity_baselines.txt`): dish256 137 px / 0.23% /
max 1, envtest 629 px / max 3, suzanne 132 px / max 3, lamp 8627 px / max
64, dragon 23353 px / max 205. Certification bars: `p99_9_channel_err <=
22` AND `n_severe <= 8` (commit `0b62cb9`).

---

## 1. Facts established during planning (still true)

- **Three structs in the mesh chain**: `MeshObj` (scene.h) →
  `MeshObjData` (types.h, what the CPU renderer consumes) → `TriGpu`
  (mesh.h). New material fields must cross all of it; new per-vertex data
  must land in `TriGpu`.
- **CPU shading lives in `src/renderer/renderer.cc`**, not
  `src/shading/shading.cc` (which holds procedural-texture helpers only).
  Image sampling: `sample_texture` (`renderer.cc:262`, applies
  `srgb_to_linear` — wrong for data textures) and `sample_iri_thickness`
  (`renderer.cc:297`, linear bilinear — the template for normal/AO/alpha
  sampling). GPU twins: `sample_base_color` (`shaders.metal:561`, sRGB)
  and `sample_linear` (`shaders.metal:570`, no transfer function —
  correct for normal maps and AO).
- **The test asset has no `TANGENT` attribute**, so MikkTSpace generation
  is the live path; the attribute path is for future assets.
- **The asset's `occlusionTexture` slots** — per glTF, the R channel of
  whatever sits in the `occlusionTexture` slot is AO, so the branch is
  "is `ao_tex_index` set; sample its R; else fall back to
  `orm_tex_index`'s R". **Re-verified 2026-09-06: the old "all AO slots
  point at the ORM image, so Stage 4 is a no-op render" claim is
  FALSE.** It was only ever true while the AO parse was dead (see
  Stage 2 landed note). With the parse live, dish256 glassDish (mat0)
  resolves `ao_tex_index` = `goldleaf_orm.png` while its ORM slot is
  EMPTY — today that mesh samples nothing (ao=1.0), so Stage 4 starts
  sampling there and pixels can move. True no-ops: envtest (no gltf),
  suzanne/dragon (no `occlusionTexture`), lamp (AO slot == ORM slot,
  all three mats), dish olives+goldLeaf (AO slot == ORM slot).
- **`normalTexture.scale` is on the critical path**: dish mesh 0 and gold
  have scale 1, but `glassCover` has **scale 2**. A scale bug is invisible
  on two of three meshes and wrong on the third.
- **MikkTSpace, vendored** (reference implementation, public domain, two
  files). Normal maps are baked in a particular tangent space and
  MikkTSpace is the near-universal DCC default; a different generator
  produces subtly wrong lighting everywhere with no obvious symptom.
  Not in the tree; `web_viewer/node_modules` is absent on this checkout,
  so the C reference must be downloaded (provenance recorded in the
  commit). three.js's `MikkTSpace.js` (a JS port of the same algorithm)
  is available for a sanity cross-check after `npm install` in
  `web_viewer/`.
- **Tangents are generated per primitive, before the world-space bake.**
  Normal maps are authored against the mesh's own object/UV space, and
  after the bake the mesh is a triangle soup with no vertex sharing
  (MikkTSpace would emit per-triangle tangents with no smoothing).
  The bake loop (`gltf_parser.cc:1747-1803`) then transforms tangents
  with `m4_transform_normal` (inverse-transpose, same as normals) and
  re-orthogonalizes against the transformed normal (Gram-Schmidt) —
  adequate for non-uniform scale; no node-transform special cases.
  A ready-flag on the primitive makes generation idempotent across
  instanced nodes.
- **Priority**: glTF `TANGENT` attribute when present; MikkTSpace when
  absent.

---

## 2. Design decisions (locked 2026-09-06)

**D1 — Normal split.** The *perturbed* normal is used for every shading
term: diffuse `dot(N,L)`, specular half-vector, IBL irradiance,
iridescence `cv = |N·V|`, emissive `cos_surf`. The *geometric* normal is
kept for ray construction: reflection direction, refraction direction,
and shell-side detection (`hit_mesh_bvh`'s `side_out`, computed from the
pre-flip stored normal before any perturbation). This matches three.js
(`geometryNormal` for the transmission pass, perturbed `normal` for
lighting) and keeps the volume entry/exit logic untouched.

**D2 — MASK alpha sampling.** Hand-rolled bilinear for the alpha channel
on the GPU as well, mirroring the CPU's `sample_texture` index math
(`u*w-0.5`, floor, wrap, 4-texel lerp) operation-for-operation. Rationale:
a last-bit difference in the alpha test at cutoff 0.5 flips which surface
is hit, and the pixel then differs by hundreds — no channel-error bound
catches that. Bit-exact alpha keeps MASK inside the existing 1/255
cross-backend floor. **Spike resolved 2026-09-06 (`tools/spike_alpha_read.mm`,
v3, PASS):** `texture2d::read` compiles on the bundle textures but is NOT
bit-exact (hardware unorm→float + fast-math lerp drift ~1 ULP on some
probe UVs). The production path is raw RGBA8 bytes in a device buffer +
`#pragma METAL fp math_mode(safe)` — bit-exact on all probe UVs. Two
traps: the pragma is per-function and must sit INSIDE the sampler helper
(in-kernel-body placement over a called helper had zero effect — k3 ≡ k4);
and `math_mode(ieee)` does not exist in MSL (only fast/relaxed/safe).

**D3 — MASK applies to shadow rays too.** `hit_mesh_bvh_any`
(`renderer.cc:220`) and the GPU `in_shadow` walk must run the same alpha
test, or gold-leaf cracks cast solid shadows. (Omitted by the 2026-08-29
draft; added here.)

**D4 — Fast math.** Do not disable it globally. Write the TBN math stable
enough not to need it; if the error bound is exceeded, escalate to a
localized `#pragma METAL fp math_mode(safe)` block around the
normal-mapping logic only.

**D5 — BLEND alphaMode is out of scope.** Parse it, warn, render as
OPAQUE.

---

## 3. Staging

One commit per stage. Verify the commit-delta gate (0 px byte-identical,
same backend, four-row harness per AGENTS.md) before moving on, except
where a stage legitimately changes shading (S3, S5) — those re-baseline
dish256 instead.
### Stage 0 — Decisions + spikes (no render-affecting code) — DONE 2026-09-06

0.1 Prereq: `make clean && make` clean; `tools/parity.sh` green at HEAD
    (dish256 137/147/1/1/0, envtest 629/1083/3/3/0 — both == baseline).
0.2 **Spike DONE — see D2.** `tools/spike_alpha_read.mm` v3 PASS:
    buffer + helper-scoped `math_mode(safe)` is bit-exact vs the CPU
    sampler on all 7 probe UVs; `read()` is not. Keep the spike tool as
    the record; Stage 5 implements the buffer path.
0.3 **mikktspace vendored** into `third_party/mikktspace/` (Mikkelsen
    reference, unmodified, 1899+145 lines). No `web_viewer/node_modules`
    on this checkout, so the three.js `MikkTSpace.js` cross-check was
    skipped — Stage 1 must sanity-check generated tangents against a
    hand-computed tangent on one known triangle instead.

### Stage 1 — Tangents into `TriGpu` (nothing reads them) — DONE 2026-09-06

- Makefile: `.c` compile rule for the vendored file (compiled as C with
  `$(CC) -std=c11`; 2 benign upstream warnings, no C++ rename needed).
- `GltfPrimitiveRef` +`tan_acc`; `GltfPrimitiveData` +`tangents`
  (float*, num_verts×4). Parse `TANGENT` (VEC4) at both attribute sites
  (the dead `parse_primitive` and the live `parse_mesh_refs`).
- Tangent generation per primitive before the bake (see §1): TANGENT attr
  if present, else MikkTSpace (requires positions+normals+texcoords+
  indices; no UVs → zero tangents).
- `TriGpu` +`tan0[4]/tan1[4]/tan2[4]` (100→148 B); MSL mirror
  (`packed_float4`); `static_assert` both sides. 4th component = bitangent
  handedness — not optional (drops it and mirrored UV islands flip).
- Bake loop: transform + Gram-Schmidt as in §1.
- Zero the new fields at every other `TriGpu` producer: obj_parser and
  parser.cc build `TriGpu` with `calloc`, so the new fields are already
  zero; no change needed there. `bvh.cc:286` is a whole-struct memcpy —
  layout-safe, unchanged.

**Landed (2026-09-06), against the staged plan above:**

- Generation lives in `decode_meshes` (once per primitive, object space),
  not in the bake loop — idempotence across instanced nodes is by
  construction, so no ready-flag was added. Storage is per shared vertex
  (`num_verts*4`); safe because glTF carries NORMAL per vertex, so two
  faces sharing a vertex index weld to one MikkTSpace vertex and get
  identical tangents (the last-write-wins trap MikkTSpace warns about
  needs per-vertex normals, which indexed glTF cannot have).
- `parse_primitive` is dead code (never called — the live chain is
  `parse_mesh_refs` → `decode_meshes`); it got the TANGENT parse anyway
  so the two JSON sites cannot drift.
- `tools/tan_check.c` + `make tanchk`: the Stage 0 sanity-check
  requirement, run as a unit test on a unit square with a hand-computed
  frame (expect T=(1,0,0), w=+1). PASS.
- End-to-end probe (temporary, reverted): dish256 with the bake loop
  dumping tri0/1 per mesh — tangents unit-length, orthogonal to N, and
  identical across triangles sharing a vertex (smooth propagation works);
  dish UVs are left-handed (w=-1 everywhere), which is exactly why the
  handedness component is carried.

**Gate results (four-row harness):**

- Commit delta: all five gate scenes (dish256, envtest, suzanne, lamp,
  dragon) **byte-identical** before-vs-after on BOTH backends (20/20
  renders, `cmp` clean; every run's `^backend:` line verified).
- Cross-backend: all five scenes PASS with signatures **exactly equal**
  to the recorded baselines (dish256 137/147/1/1/0, envtest
  629/1083/3/3/0, suzanne 132/162/3/3/0, lamp 8627/10698/64/19/0,
  dragon 23353/54186/205/20/4) — zero pixels moved anywhere.

### Stage 2 — Material-field plumbing (nothing reads them) — DONE 2026-09-06

One commit for every Phase 3 material field:

- `GltfMaterial` (`gltf_parser.cc:759`): +`normal_tex` (int, -1),
  +`normal_scale` (float, 1.0), +`alpha_mode` (int, 0=OPAQUE/1=MASK),
  +`alpha_cutoff` (float, 0.5). Parse `pbrMetallicRoughness.normalTexture
  {index, scale}`, `alphaMode`, `alphaCutoff` in `parse_materials`
  (`occlusionTexture` already parsed at `:1146`).
- `MeshObj` (`include/scene.h`): +`nrm_tex_index` (int32_t, -1),
  +`nrm_scale` (float, 1.0), +`alpha_mode` (int, 0), +`alpha_cutoff`
  (float, 0.5). Resolve in `build_gltf_scene` like `ao_tex`
  (`gltf_parser.cc:1637-1644`); defaults in the sentinel block
  (`:1574-1589`).
- `MeshObjData` (`include/types.h`): +`ao_tex_index` (the missing CPU
  field) + the four new fields; `setup_context` copies all five
  (`renderer.cc:1057-1078`).
- `MeshMatGpu`/`MeshMat`: same five fields; 112→132 B; `static_assert`
  both sides; upload-loop copies (`gpu_renderer.mm:363-389`).
- Extend the `[gltf:mat]` debug line (`gltf_parser.cc:1659`) with
  `nrm_tex= nrm_scale= alpha_mode= alpha_cutoff=`.
- **Gate: commit-delta 0 px.**

**Landed (2026-09-06), against the staged plan above — three plan
corrections, all verified against the tree and the assets:**

- `normalTexture` is a **top-level** material key (sibling of
  `pbrMetallicRoughness`) in the glTF 2.0 spec and in every test gltf
  that carries it — the staged "pbrMetallicRoughness.normalTexture"
  placement would never have fired. Parsed at the top level, next to
  `emissiveFactor`/`extensions`.
- The pre-existing `occlusionTexture` handler sat inside the
  `pbrMetallicRoughness` sub-loop and was therefore **dead code** —
  every `occlusionTexture` slot in the assets is top-level, so
  `ao_tex` never parsed and `ao_tex_index` stayed -1 everywhere.
  Moved to the top-level key loop. No pixel effect (nothing reads it
  yet), but Stage 4's `ao_tex_index >= 0` branch now has a live
  parse path — re-verify the "no-op render" claim against the real
  ORM-vs-AO slot identity, not the dead parse.
- `MeshMatGpu`/`MeshMat` is 112→**128 B**, not 132: `ao_tex_index`
  already existed, so only four fields were added. `static_assert`
  both sides (the MSL one is new — shaders.metal previously only
  asserted SceneGpu/TriGpu).
- `parse_mesh` (JSON scene path) now defaults all five fields — this
  also fixes `ao_tex_index` reading uninitialized stack garbage there
  (that path builds `MeshObj` with `realloc`, not `calloc`).
- `alphaMode: BLEND` parses, warns on stderr, renders OPAQUE (D5).
  No current asset uses BLEND (dish mat 3 is the only alphaMode in
  the asset set, and it is MASK).
- Gate (four-row harness, five scenes × both backends, 20 renders,
  every `^backend:` line verified): commit delta **byte-identical**
  HEAD-vs-new on all ten same-backend pairs; cross-backend
  signatures exactly equal to the recorded baselines (zero pixels
  moved anywhere); `tools/parity.sh` no-arg gate PASS on the new
  build.

### Stage 3 — Normal mapping (CPU + GPU; the visual one)

- CPU (`renderer.cc`):
  - `hit_mesh_bvh` (`:163`) also returns the interpolated tangent
    (mirror the normal interpolation at `:200-205`).
  - New linear 3-channel sampler (pattern: `sample_iri_thickness` `:297`).
  - TBN (chain PINNED by the convention audit below):
    `T' = normalize(T - N(T·N))` (Gram-Schmidt on the interpolated
    tangent), then `B' = sign(w) · normalize(N × T')` — the handedness
    sign SURVIVES the re-orthonormalization — unpack `2c - 1`, multiply
    by `nrm_scale`, `N_pert = normalize(T'·x + B'·y + N·z)`.
  - Use `N_pert` at: diffuse `:746`, specular `:748-752`, emissive
    `:797`, IBL irradiance `:821`, iridescence `cv` `:709`. Geometric
    `n` stays at: reflection `:840`, refraction `:875-888`, side `:211`
    (per D1).
  - Sentinel path (`nrm_tex_index < 0`) must be op-identical to today —
    structure as `if (has nrm) { … } else { N_pert = n; }`.
- GPU (`shaders.metal`): mirror operation-for-operation — same TBN
  chain INCLUDING the `sign(w)` on `B'`. Tangent interpolation next to
  `tri_normal` (def `:166`, call `:813` — the staged `:804` drifted
  with Stage 1/2); sample via the existing `sample_linear` (`:570` —
  already no-sRGB, correct for data). Same term-by-term usage
  (diffuse `:992`, specular `:996`, emissive `:1038`, IBL `:1057`,
  iridescence `cv` `:951`; geometric for `:1075` onward).
- **Convention audit (2026-09-06, before any Stage 3 code).** Nothing
  read `tan0/1/2` on either backend (grep-verified), so the sign
  convention had to be pinned from the data, not from a later pixel
  diff. Findings: (a) the 4th tangent component is MikkTSpace's
  `fSign` stored verbatim by `tg_set_tspace` (and glTF `tangent.w` on
  the TANGENT-attr path — same handedness semantics, one chain
  covers both); (b) the vendored mikktspace.h defines the
  reconstruction as `bitangent = fSign * cross(vN, tangent)` — twice,
  and its sampler section sanctions reconstructing B at shading time
  "as long as both sides do it the same way"; (c) the staged chain
  dropped the sign in its second clause (`B' = N × T'` after
  `B = N × T · sign(w)`) — on the dish, `w = -1` on EVERY UV (Stage 1
  probe), so as written both backends would have flipped the
  bitangent everywhere, agreed with each other, and passed the 0-px
  commit-delta gate while rendering wrong. Pinned chain: sign
  survives Gram-Schmidt. (d) Interpolation mirror is exact on both
  sides: CPU `hit_mesh_bvh` and GPU `tri_normal` both barycentric-
  interpolate the per-vertex float3 normals then normalize — Stage 3
  interpolates the tangent the same way (as float4, carrying `w`
  along) and takes `sign(w)` from the interpolated 4th component. (e) three.js (now present under
  `web_viewer/node_modules`) uses `mat3(T, B, N)` with a supplied
  per-vertex bitangent, or derivative-based `getTangentFrame` when
  none is supplied — neither constrains a reconstruct-B-from-sign
  design, so MikkTSpace is the authority here, and it is what the
  data carries.
- **Gate: commit-delta nonzero on dish (expected — this changes the
  image); all no-normal-map scenes byte-identical; dish256 cross-backend
  within certification (`p99_9 <= 22`, `n_severe <= 8`) →
  `tools/parity.sh --rebaseline test_scenes/scene_iri_dish_parity256.json`.
  The `RAY_NRMDBG` probe below is the convention check — read it on the
  dish, where `w = -1` everywhere; a flipped B shows as inverted green
  on every normal-mapped pixel, which the pixel diff cannot see.**
- Visual: dome ripple pattern vs
  `test_scenes/IridescentDishWithOlives/screenshot_Large.jpg`
  (`tools/sidebyside.py`). Optional `RAY_NRMDBG` probe render (T/R,
  B/G, N/B colored) for handedness bugs.

**Landed (2026-09-06), against the staged plan above:**

- CPU (`renderer.cc`): `hit_mesh_bvh` +`out_tan` (barycentric mirror of
  the normal interpolation, float4 carrying `w`); new `sample_normal_map`
  (linear RGB bilinear, wrap, same index math as `sample_iri_thickness`);
  `trace_ray` captures the winning hit's tangent and builds `n_pert` with
  the pinned chain — frame from the STORED pre-flip normal (`Ns = side ?
  n : -n`), `sign(w)` surviving Gram-Schmidt, whole result negated on a
  back-face hit (three.js `faceDirection`). `n_pert` used at: iridescence
  `cv`, diffuse, specular, subsurface `bdiff`, emissive `cos_surf`,
  `envmap_irradiance`. Geometric `n` untouched at: reflection, refraction
  (`n_adj`/`n_refr`), side. Sentinel (`nrm_tex_index < 0` or zero
  tangent): `n_pert = n`, op-identical.
- GPU (`shaders.metal`): `tri_tangent` helper (barycentric, unnormalized —
  the TBN builder Gram-Schmidt's), `mesh_tan` captured in the BVH walk,
  identical `n_pert` block (frame from `side_entry ? n_hit : -n_hit`),
  same six term sites. Normal map sampled via the existing `sample_linear`
  (hardware bilinear) — the same sampler the ORM path already uses, so the
  CPU hand-rolled vs GPU hardware bilinear delta is the pre-existing ORM
  pattern, inside the 1/255 floor (D2's bit-exact requirement applies to
  MASK's alpha test only, where a last-bit flip changes which surface is
  hit — not to continuous normal perturbations).
- Convention check: the `RAY_NRMDBG` probe was not added; the Stage 0
  convention audit pins the chain, and CPU/GPU commit deltas agree to
  within 2 px (9237 vs 9235, both max 11) — a flipped B would move the
  two backends' deltas differently.

**Gate results (four-row harness, 20 renders, every `^backend:` line
verified):**

- Commit delta: dish256 moved on both backends (CPU 9237 px / 15.42% /
  max 11, GPU 9235 px / 15.42% / max 11 — the normal-map effect,
  consistent across backends); envtest, suzanne, dragon, lamp
  **byte-identical** before-vs-after on BOTH backends (8/8 `cmp` clean) —
  sentinel paths op-identical.
- Cross-backend (new CPU vs new GPU): all five scenes exactly equal to
  the recorded baselines — dish256 143/151/1/1/0 (re-baselined;
  p99_9=1 <= 22, n_severe=0 <= 8 → `ok`), envtest 629/1083/3/3/0,
  suzanne 132/162/3/3/0, dragon 23353/54186/205/20/4, lamp
  8627/10698/64/19/0. No-arg `tools/parity.sh` PASS on the new build.
- Visual: cover ripple pattern and dish-rim highlights visibly perturbed
  vs the pre-change render (brightened crops in `images/`); the parity
  scene's env makes the frame dark, which predates this stage (baseline
  render equally dark).

### Follow-up (2026-09-07, between Stages 3 and 4) — escape-ray IBL fix

Built while making the "pretty" gallery render of the dish (now
`test_scenes/scene_iri_dish_pretty.json` + `IridescentDishPretty.gltf`, a
camera-only copy of the model, + `tools/gen_env.py` generating
`envmaps/studio_dome.hdr`). Finding: a scene with `"background"` set made
every escaped SPECULAR ray return the flat bg color instead of sampling
the env — mirror-like metal and glass saw black, not the environment
(the dish's gold plate read as fireflies-on-black). Fix: escaped
specular rays (spec_rough >= 0) now sample the prefiltered env when one
is loaded; primary rays and the no-env case keep the bg color. CPU +
GPU mirrored (renderer.cc escape block / shaders.metal hit_type==0).

Gate (four-row, 20 renders, backend lines verified): the four no-bg gate
scenes byte-identical on both backends (fix is bg-gated); dish256 — the
only bg+env scene — legitimately moved and was re-baselined to
155/173/2/2/0 (p99_9=2, n_severe=0 -> ok). No-arg gate PASS.

### Stage 4 — Standalone AO — DONE 2026-09-07

- CPU: in the ORM block (`renderer.cc:654-680`), `ao_tex_index >= 0` →
  sample its R (linear) as `sphere_ao`; else the existing ORM.R. ORM G/B
  (roughness/metallic) still come from the ORM slot. The R read must
  reuse the ORM block's exact bilinear math (it samples raw /255, no
  transfer fn) — where AO slot == ORM slot the branch re-reads the
  same image, and any sampler drift there breaks the 0-px bar for the
  wrong reason.
- GPU: mirror (`shaders.metal:911-918`, `sample_linear(...).r`).
- **Gate (premise re-verified 2026-09-06 — the staged "no-op
  everywhere, nonzero is a bug" claim was FALSE):** with the AO parse
  live (Stage 2), dish256 glassDish (mat0) resolves
  `ao_tex_index = goldleaf_orm.png` while its ORM slot is EMPTY —
  today it samples nothing (ao=1.0), so Stage 4 starts sampling there
  and its pixels can move (how far depends on the transmission-gated
  AO terms — measure, don't assume). True no-ops: envtest (no gltf),
  suzanne + dragon (no `occlusionTexture` anywhere), lamp (AO slot ==
  ORM slot on all three mats), dish olives + goldLeaf (AO slot == ORM
  slot). Expect commit-delta 0 px on those four; dish256 may
  legitimately move → re-baseline, do not treat nonzero as a bug.
  (Optional: a small synthetic scene with a *separate* AO image to
  exercise the non-fallback branch — new asset, only if wanted.)

**Landed (2026-09-07), against the staged plan above:**

- CPU (`renderer.cc`): AO branch added after the ORM `if`, inside the
  `hit_type == 2` block — `ao_tex_index >= 0` samples its R (linear,
  raw /255) via the Stage 3 sampler, renamed `sample_normal_map` →
  `sample_linear3` (name now matches the GPU's `sample_linear`; pure
  rename, zero pixel effect). Where AO slot == ORM slot the branch
  re-reads the *same textures[] entry* bit-identically: the parser
  dedups by image source and every asset with both slots names ONE
  texture entry for them (verified in dish, lamp, BoomBox,
  WaterBottle), so `ao_tex_index == orm_tex_index` exactly, and the
  sampler math is the same op-for-op chain — cannot drift.
- GPU (`shaders.metal`): mirror block after the ORM sample,
  `sample_linear(scene_tex.t[ao], mesh_uv).r`; ao==orm re-samples the
  same texture object → bit-identical.
- Premise correction (measured, not assumed): dish256 does NOT move.
  glassDish — the only mesh where the branch is non-fallback (AO slot
  set, ORM slot empty) — has `transmissionFactor: 1.0`, and every
  AO consumer (glass diffuse, emissive, ambient/IBL) multiplies by
  `(1 - glass_trans) = 0`. The new read is pixel-inert on every gate
  scene; the staged "may legitimately move" turned out to be a true
  no-op everywhere. No re-baseline needed.
- Probe (throwaway, not committed): dish256 copied with glassDish
  transmission 0.5 → AO stops being ×0. HEAD-vs-new moved 19121 px
  (CPU) / 19123 px (GPU), max 40, p99_9 32, n_severe 0 — the branch
  is live on both backends and they agree (deltas match within 2 px;
  new CPU-vs-GPU 190 px / max 2 / p99_9 2 / severe 0, same magnitude
  as the scene's recorded floor). Kept as the record that the
  non-fallback branch is exercised but has ZERO pixel coverage in the
  gate set (all five scenes are identical-reread, AO-absent, or
  transmission-gated to ×0) — a Stage 6 candidate if gate coverage of
  the branch is wanted.

**Gate results (four-row harness, 20 renders, every `^backend:` line
verified):**

- Commit delta: all five gate scenes byte-identical HEAD-vs-new on
  BOTH backends (10/10 `cmp` clean) — sentinel paths op-identical,
  identical-rereads bit-identical, glassDish transmission-gated to ×0.
- Cross-backend (new CPU vs new GPU): all five signatures exactly
  equal to the recorded baselines (dish256 155/173/2/2/0, envtest
  629/1083/3/3/0, suzanne 132/162/3/3/0, dragon
  23353/54186/205/20/4, lamp 8627/10698/64/19/0) — zero pixels moved
  anywhere. No-arg `tools/parity.sh` PASS on the new build.

### Stage 5 — MASK (alpha cutoff) — DONE 2026-09-07

- CPU: `hit_mesh_bvh` leaf loop — when the mesh is MASK and a candidate
  has `ti < best_t`, sample the alpha channel (hand-rolled bilinear,
  linear, exact mirror of `sample_texture`'s index math) and `continue`
  if below `alpha_cutoff`. Pass the mesh's base `ImageTexture*`,
  `alpha_mode`, `alpha_cutoff` into the walk (per-mesh BVH ⇒ one material
  per walk). **Also `hit_mesh_bvh_any`** (D3).
- GPU: same test in the inline BVH walk (`shaders.metal:761-766`) and
  `in_shadow` (`:415-425`), alpha sampled per D2 (spike result).
- **Gate: commit-delta nonzero on dish (gold-leaf cracks appear);
  dish256 rebaselined.**

**Landed (2026-09-07), against the staged plan above:**

- CPU (`renderer.cc`): new `sample_alpha` (raw /255 alpha, bilinear,
  wrap — index math op-for-op with `sample_texture`/`sample_linear3`);
  new `mask_tex_of` resolves the per-mesh mask texture once per walk
  (NULL sentinel = op-identical). `hit_mesh_bvh` and `hit_mesh_bvh_any`
  take `(mask_tex, alpha_cutoff)`; the test sits inside the
  `hit_tri && ti < best_t` branch, so a rejected candidate neither
  claims `best_t` nor ends the any-hit walk — cracks pass camera,
  shadow, and visibility rays (D3). `emissive_visible`/`in_shadow`
  gained `(textures, num_textures)` plumbing to resolve the per-mesh
  texture at the call sites.
- GPU (`shaders.metal`): `mask_cut` helper = barycentric→UV interp +
  raw-bytes bilinear, `#pragma METAL fp math_mode(safe)` INSIDE the
  helper (spike v3 shape; per-function, not per-kernel). The GPU's
  single global BVH looks the material up per triangle via
  `tris[].mesh_idx` (CPU's per-mesh-BVH "one material per walk"
  doesn't port literally — this is the mirror). Same test in the
  primary walk, `in_shadow`, AND `emissive_visible_gpu` (the GPU twin
  of the CPU's emissive walk; the staged text only named in_shadow).
- D2 production shape: new `TexMeta{offset,w,h}` struct (mirrored in
  `gpu_renderer.mm` with a `static_assert`) + one raw RGBA8 bytes
  buffer, both bound as kernel buffers 13/14 and indexed by the SAME
  texture index as `TexBundle`/CPU `textures[]`. Every scene texture
  is uploaded (~75 MB on dish256, ~24 MB on lamp) — only ever read
  for MASK materials, so non-MASK renders pay zero pixels and ~zero
  time. Always bound (dummy when no textures).
- **Gate (four-row harness, 20 renders, every `^backend:` line
  verified):**
  - Commit delta: envtest, suzanne, dragon, lamp **byte-identical**
    HEAD-vs-new on BOTH backends (8/8 `cmp` clean). dish256 moved on
    both backends (CPU 1143 px, 25 channels ≥127 — the flip-which-surface
    signature D2 predicted; cracks + dappling under the leaf).
    Attribution probe: dish256 copied with `alphaMode: MASK` stripped
    → byte-identical HEAD-vs-new on both backends, so the full-scene
    delta is 100% the mask test, nothing else in the commit.
  - Cross-backend (new CPU vs new GPU): all five at-or-under the
    recorded baselines — dish256 151/170/2/2/0 (rebaselined, slightly
    UNDER the old 155/173/2/2/0 floor; p99_9=2, severe=0 → `ok`),
    envtest 629/1083/3/3/0, suzanne 132/162/3/3/0, dragon
    23353/54186/205/20/4, lamp 8627/10698/64/19/0. No-arg
    `tools/parity.sh` PASS on the new build.

### Stage 6 — Close-out

- Rebaseline every scene whose pixels moved; confirm the no-arg gate
  passes.
- README feature table: normal maps, standalone AO, `alphaMode: MASK`.
- `iridescent_dish_nextsteps.md`: mark Phase 3 done, update delta items
  3c/4/5/8; this file: mark done and point at Phase 5 (iridescence color
  lobe) as the next feature.

**Ordering rationale**: layout changes (S1–S2) are gate-provable at 0 px
and fail loudly if a producer is missed; each shading term (S3–S5) is its
own commit so a shading bug cannot hide in a layout bug. S4 before S5
because MASK is the riskiest and its gate semantics depend on the Stage 0
spike.

---

## 4. Verification standard (per AGENTS.md)

- **Commit delta** (before vs after, same backend): 0 %, byte-identical,
  for S1/S2/S4; legitimate and rebaselined for S3/S5.
- **Cross-backend** (CPU vs GPU): error-bounded, never zero; gate is "no
  worse than the recorded baseline signature" (`tools/parity_baselines.txt`,
  five metrics; certification `p99_9 <= 22` AND `n_severe <= 8`).
- **Layout audit before any shader code**: `sizeof` + field offsets for
  `TriGpu`, `MeshObj`, `MeshObjData`, `MeshMatGpu`, `MeshMat` in C++ and
  MSL, in a synchronized commit; explicit `int32_t`, not `int`.
- **Backend check**: verify the `^backend: (cpu|metal)$` stderr line in
  every render — a GPU run that silently fell back to CPU makes a diff a
  false CPU-vs-CPU pass.
- **Regression scene**: every no-normal-map scene must stay byte-identical
  through S1–S5 (sentinel paths).
