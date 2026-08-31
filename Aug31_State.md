# Reconstructed State — HEAD `d31b10c` (branch `phase2-iridescence-wip`)

Reconstructed 2026-08-31 by verifying the Aug30 docs and `nextsteps.md` against
the tree at HEAD. Where a doc and the code disagree, the code wins. Line
citations below are from HEAD, not from the planning docs.

## What's actually done (verified against HEAD)

- **Phase 2 IBL** — prefiltered-env specular + band-0..1 SH diffuse irradiance,
  both backends. CPU wiring `src/renderer/renderer.cc:546-556` (specular) and
  `:818-827` (irradiance ambient); GPU mirror `src/renderer/shaders.metal:779`
  and `:810`. Closed `ace5b97`.
- **`ao_tex_index` plumbing** — end-to-end but **no shading term reads it**:
  parse `src/parser/gltf_parser.cc:1146-1159` + resolve `:1637-1642`, field
  `include/scene.h:58` (last field, offset 148, `sizeof(MeshObj)` 152), GPU
  copy `src/renderer/gpu_renderer.mm:360`, `static_assert(sizeof(MeshMatGpu)==112)`
  `src/renderer/gpu_renderer.mm:162`. Landed `74acd37`.
- **Phase 4 GPU parity fix** — multi-texture argument-buffer bind + shadow
  `skip_mesh` index fix (`ef63cfd`). The "byte-identical" result it recorded was
  a **false positive** (see inconsistent item 1/2).
- **Metal page-fault fix** — `useResources` on the 64-slot texture bundle
  (`83d6230`). This is the first *honest* CPU/GPU measurement in the tree.
- **SceneGpu layout re-sync** — `dbg_x/dbg_y` added to the MSL struct,
  `static_assert(sizeof(SceneGpu)==132)` `src/renderer/gpu_renderer.mm:160`
  (`a0f598f`).
- **Parity harness** — AGENTS.md four-row gate + `tools/parity.sh` with the
  `^backend:` check (`0b78da4`, `a423182`).
- **Phase 1 scene foundation** — opt-in floor + background color, both backends.
- **3 latent bug fixes + broken asset** — glTF camera-fov slot, HDR blank-line
  header scan, env upload row-pitch repack; `envmaps/polyhaven_haven_01_1k.hdr`
  is still an HTML page (a botched download, not re-fetched).

## What's genuinely left (verified NOT done against HEAD)

- **Phase 3 Step 1 — `TriGpu` expansion + MikkTSpace.** `TriGpu` is still
  **100 bytes** (`include/mesh.h:4-9`; MSL mirror `src/renderer/shaders.metal:97-102`).
  No `mikkcspace.*` anywhere in the tree (only a three.js copy under
  `web_viewer/node_modules/`). Not started.
- **Phase 3 Step 2 — normal mapping.** `normalTexture` appears **nowhere** in
  `src/`. Not started.
- **Phase 3 Step 3 — AO shading.** `ao_tex_index` plumbed but unread (above).
- **Phase 3 Step 4 — MASK (`alphaMode`).** `alphaMode`/`alphaCutoff` appear
  **nowhere** in `src/`. Not started.
- **§5 alpha-sampling decision for MASK** — not made.
- **Retire TEMP-DBG** — still in the tree, gated behind `RAY_GDEBUG`/`RAY_TEXDBG`/
  `RAY_RAWDBG`: `src/renderer/shaders.metal:79-80`, `src/renderer/gpu_renderer.mm:118-119,440-442,478-482,556-565,591,617-648`.
- **Re-download a real `haven_01` HDR / re-baseline the dragon** — not done
  (`envmaps/polyhaven_haven_01_1k.hdr` is still HTML).
- **The parity prerequisite check (run `tools/parity.sh` at HEAD)** — never run
  in a prior session, and it **FAILS** (see inconsistent item 1). The Aug30 docs
  named this as the required first step; its result was never recorded.

## Abandoned / inconsistent things in the tree

1. **The parity gate FAILS at HEAD.** `tools/parity.sh` requires `differing=0`
   (`tools/parity.sh:52`) on its default pair, but the honest numbers are:
   `scene_iri_dish_parity256` = **137 px / 0.23%** (the legitimate recorded
   cross-backend floor, `AGENTS.md`) and `scene_envtest_stdout` =
   **19,603 px / 4.08% / max err 255** (a *real* divergence — max 255 means one
   backend is black where the other is bright). The harness's baked-in
   "byte-identical" assumption is false for both default scenes.
2. **Four different "current lamp baseline" numbers, all wrong except the code's.**
   `README.md:110-111` "0 differing pixels — byte-identical";
   `iridescent_dish_nextsteps.md:144-148` "byte-identical (0 px)";
   `Aug30_Overall_NextSteps.md:7,21` "lamp scene is byte-identical";
   `nextsteps.md:565,691` "39,875 (5.07%)". The code (ground truth, commit
   `83d6230` body) records the honest number: **lamp 3.45%**. Per code-wins, the
   lamp is ~3.45% cross-backend at HEAD.
3. **The envtest 0.06%/max-2 claim is refuted.** `iridescent_dish_nextsteps.md`
   setup-fix #3 records envtest at "278 px (0.06%) / max 2"; the honest HEAD
   number is **4.08% / max 255**. A real CPU/GPU env-sampling divergence was
   either never fixed or was masked by the CPU-vs-CPU false positive.
4. **Orphaned baseline worktree** `/private/tmp/ray-baseline` (detached at
   `d31b10c`, marked prunable) — leftover from the parity harness, not removed.
5. **Stale stash** `stash@{0}` on `main` — an uncommitted BVH SAH experiment
   (`SAH_BINS 12→16`, depth early-termination), never committed or dropped.
6. **Unmerged branch** `fix/png-crc-checkerboard-parser-crash` (1 commit,
   `4eb3e0f` "Move camera back to original position") — not in `main` or the
   current branch.
7. **Untracked session notes** `Aug30_Overall_NextSteps.md` +
   `Aug30_Step1_Details.md` — never committed.
8. **TEMP-DBG leftover with a debug hack** — `src/renderer/gpu_renderer.mm:620`
   `if (sc.dbg_x == 271 || 1)` is an always-true leftover condition (harmless,
   gated by `RAY_GDEBUG`, but a trace of half-finished work).
9. **`main` is 12 commits behind** the current branch — all Phase 2/3 work is
   unmerged into `main`.
