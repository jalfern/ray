# AGENTS.md

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
