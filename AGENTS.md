# AGENTS.md

macOS ray tracer (`./ray2`) with parallel CPU and Metal GPU backends, glTF/OBJ
import. Correctness is measured, not eyeballed: CPU/GPU pixel parity is the
gate (see "Render parity" below).

## Build and run

- macOS-only C++11 (`g++`, links Metal/Foundation/zlib); no package manager, `include/stb_image.h` is vendored.
- `make` → `./ray2`. `make run` renders `scenes/scene.json`. `make test` renders six smoke scenes (runs only — no assertions).
- Backend is GPU-first with loud CPU fallback. `--cpu` forces CPU; also `--threads N`, `--mesh-stats`, `--tri-debug`.
- Scene JSON: `mesh`/`gltf` paths resolve relative to the scene file's directory; `environment.file` resolves relative to the **CWD** — run from the repo root.
- glTF test models and the `*_stdout.json` parity scenes live in `test_scenes/`, not `scenes/`.
- A scene without an `"output"` key writes raw PPM to stdout, preceded by a `GPU`/`28T` prefix line; `tools/ppm_diff.py` scans for `P6\n`.

## CPU/GPU mirror — read before touching any shading term

- `include/thin_film.h` and `include/volume.h` are the source of record for **both** backends; `src/renderer/shaders.metal` is a line-for-line / operation-for-operation MSL port. A shading change must be edited on both sides or parity breaks.
- GPU buffer structs (`SphereGpu`, `SceneGpu`, …) are duplicated between `gpu_renderer.mm` and `shaders.metal` with `static_assert` on sizes in the `.mm`. Change both sides and the asserts together. `MAXTEX`/`RAY_MAXTEX` = 64 must match in both files.
- `shaders.metal` is embedded as a C string into `build/renderer/shader_src.h` and compiled at runtime (`newLibraryWithSource`) — no `metal` CLI needed, but shader edits require a rebuild.
- `web_viewer/` is a separate Vite+TypeScript+three.js app (`npm run dev` there); its `node_modules/three` shader chunks are the reference model for the thin-film/volume ports.

## Testing and parity

- `tools/parity.sh [SCENE ...]` is the CPU/GPU gate; it does not build — run `make` first. With no args it gates the default set (iri dish 256 + envtest). The no-arg gate PASSES at HEAD. (envtest was the lone `known-bug` — a GPU env-sampling divergence, max_channel_err=255 — fixed 2026-09-04 by reading the CPU mip chain from a buffer in the prefiltered path; re-baselined to `ok`, max 3. See `iridescent_dish_nextsteps.md`.)
- `make volcheck` runs the KHR_materials_volume math parity check — needs `node` (`tools/vol_ref_check.mjs`).
- `test_scenes/lamp_glass_mask.ppm` is committed (`*.ppm` is gitignored; it was force-added). Use the committed file, do not regenerate it.
- `make models` regenerates the procedural meshes in `models/` — regenerate, never hand-edit.
- Working plan docs are the record of project state (rebaseline history, open bugs): `glass_parity_nextsteps.md` (next work item — glass light-transport CPU/GPU divergence), `iridescent_dish_nextsteps.md` (active feature plan), `dragon_nextsteps.md`, `nextsteps.md` (lamp history). See README "Next Steps".

## Render parity (Tier 1 gate)

Two different bars — never conflate them:

- **Commit delta** (before vs after, same backend): 0%, byte-identical.
  A commit that touches no shading term cannot change a pixel. This is
  enforceable; any nonzero delta is a bug in the commit.
- **Cross-backend** (CPU vs GPU): error-bounded, never zero. The gate is
  "no worse than the recorded baseline signature", not "zero". The
  backends differ by ~1/255 float-rounding on a small minority of pixels.
  Baselines live in `tools/parity_baselines.txt`; see the section below.

### The four-row harness

For any render-affecting commit, render the scene four ways — new code
CPU, new code GPU, pre-change (HEAD) CPU, pre-change (HEAD) GPU — and
diff with `tools/ppm_diff.py`:

| # | Comparison | Bar |
|---|---|---|
| 1 | new CPU vs new GPU | ≤ recorded cross-backend baseline (per scene) |
| 2 | baseline GPU vs new GPU | 0%, byte-identical (unless the commit changes shading) |
| 3 | baseline CPU vs new CPU | 0%, byte-identical (unless the commit changes shading) |
| 4 | baseline CPU vs baseline GPU | = recorded baseline signature |

Rows 2–3 are the commit-delta proof; row 1 is the cross-backend gate.
Row 4 establishes the floor; when rows 2–3 are 0 it follows from rows
1–3 by transitivity and need not be rendered separately. When a
shading commit legitimately moves pixels, rebaseline with
`tools/parity.sh --rebaseline SCENE` — it records the new signature in
`tools/parity_baselines.txt` and the gate follows.

Harness notes:

- Baseline = clean worktree at HEAD: `git worktree add /tmp/ray-baseline HEAD && make -C /tmp/ray-baseline`, render from inside it (asset paths resolve relative to the scene file's dir), then `git worktree remove`.
- Always verify the `^backend: (cpu|metal)$` line in each run's stderr; a GPU run that silently fell back to CPU makes a diff a false CPU-vs-CPU pass. `tools/parity.sh` does the new-CPU/new-GPU pair plus this backend check.
- Rerunning the same binary is deterministic on a given machine, so one render per configuration is enough.

### Cross-backend baselines

Source of truth: `tools/parity_baselines.txt` — one line per scene,
`status  scene  differing  sum_abs_err  max_channel_err`. Do not copy the
signatures into docs; the file is the record and `tools/parity.sh` reads it,
so a restated table here would just drift.

Gate semantics (`tools/parity.sh`):

- **Regression rule.** A scene with an `ok` baseline PASSes iff the current
  CPU-vs-GPU render is `<=` the baseline on all three metrics (differing,
  sum_abs_err, max_channel_err). Reruns are deterministic on one machine, so
  any metric above baseline is a regression -> FAIL.
- **Certification bar.** `--rebaseline` writes `ok` only when
  `max_channel_err <= FLOOR_MAX` (127), else it records `known-bug`. 127 is
  "no pixel reaches half the 8-bit range": a legitimate floor never puts a
  pixel at >=128/255 in any channel, but a real structural bug (one backend
  black where the other is bright) does. See the FLOOR_MAX commit for the
  histogram derivation.
- **known-bug.** A recorded CPU/GPU divergence above the certification bar.
  The gate FAILS on it (it is not a floor) until the divergence is fixed and
  the scene is re-baselined to `ok`.
- **No baseline.** A scene with no line in the file FAILs; certify it with
  `tools/parity.sh --rebaseline SCENE`.
