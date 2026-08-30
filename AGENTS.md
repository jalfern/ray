# AGENTS.md

## Render parity (Tier 1 gate)

Two different bars — never conflate them:

- **Commit delta** (before vs after, same backend): 0%, byte-identical.
  A commit that touches no shading term cannot change a pixel. This is
  enforceable; any nonzero delta is a bug in the commit.
- **Cross-backend** (CPU vs GPU): error-bounded, never zero. The gate is
  "no worse than the recorded baseline signature", not "zero". The
  backends differ by ~1/255 float-rounding on a small minority of pixels.

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
shading commit legitimately moves pixels, rebaseline: record the new
row-4 signature below and update the gate.

Harness notes:

- Baseline = clean worktree at HEAD: `git worktree add /tmp/ray-baseline HEAD && make -C /tmp/ray-baseline`, render from inside it (asset paths resolve relative to the scene file's dir), then `git worktree remove`.
- Always verify the `^backend: (cpu|metal)$` line in each run's stderr; a GPU run that silently fell back to CPU makes a diff a false CPU-vs-CPU pass. `tools/parity.sh` does the new-CPU/new-GPU pair plus this backend check.
- Rerunning the same binary is deterministic on a given machine, so one render per configuration is enough.

### Recorded cross-backend baselines

| Scene | Pixels | Differing | % | sum_abs_err | max err |
|---|---|---|---|---|---|
| test_scenes/scene_iri_dish_parity256.json (256×234) | 59,904 | 137 | 0.23% | 147 | 1/255 |

Recorded 2026-08-29, pre-change signature for the `ao_tex_index` plumbing commit (verified byte-identical after, both backends).
