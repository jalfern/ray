# Glass light-transport CPU/GPU parity — investigation & fix plan

Status: **open** (next work item after the envtest fix, 2026-09-04).
Owner: fresh session. Read `AGENTS.md` first (build, parity gate, CPU/GPU
mirror rule, four-row harness).

## Objective

Find and fix the structural divergence between the CPU's **recursive**
`trace_ray` and the GPU's **iterative (stack-based)** `trace_ray` on
transmission/glass paths. It drives two certified floors:

- `scene_lamp_stdout.json`   — 27166 px (3.45%) / sum 167753 / **max 80**
- `scene_dragon_stdout.json` — 46688 px (5.94%) / sum 185981 / **max 53**

Success = both re-baselined to a strictly lower floor (max_channel_err still
≤ 127, i.e. still `ok`), with the root cause understood — not just
re-baselined. The no-arg gate must still PASS and no other scene may regress.

## Why this one (context)

The envtest bug (the only `known-bug`) is fixed; the whole gate is green. The
remaining floors are all `ok` (certified, not blocking). This is the highest
value left because it is a **real structural bug** (two backends compute glass
light transport differently) and it pays off on **two** scenes at once. The
suzanne floor (13.88% but max 27) is a larger percentage but a smaller
per-pixel error and an untextured-path gap — lower value per effort.

## The two implementations (where to look)

- **CPU** — `src/renderer/renderer.cc`, `trace_ray` (line 453). Recursive.
  - Depth cap: `if (depth > MAX_DEPTH) return 0` (line 460); `MAX_DEPTH = 4`
    (line 26). So depths 0..4 are shaded, depth 5 → black.
  - Reflection recursive call: line 842 (`depth + 1`).
  - Refraction recursive call: line 934 (`depth + 1`), gated on `k > 0` (TIR).
  - Fresnel: scalar `r0 + (1-r0)*(1-cos_i)^5` (lines 943-945); iridescence
    replaces the surface weight per channel (comment at 947+).
  - Medium carried as the `Medium med` parameter; refracted-ray medium set at
    lines 909-916 (front/back `side` test, gated on `vol_th > 0`).
  - In-medium segment loss `Tseg` multiplies base/refl/refr (lines 831, 847,
    940).
- **GPU** — `src/renderer/shaders.metal`, `trace_ray` (line 687). Iterative.
  - `MAX_DEPTH = 4` (line 11). Stack arrays `stk_o/d/th/md/ma/dp/sr`
    (lines 700-706); `stk_md` packs the medium as `float4(ior, cr, cg, cb)`,
    air = `(1,1,1,1)`.
  - Outer `while (stk > 0)` (line 720) pops a frame; inner
    `for (depth = dp0; depth <= MAX_DEPTH; depth++)` (line 736) bounces it.
  - **Key structure:** on a glass hit the *reflected* ray **continues the
    current frame's inner loop** (`ro/rd/thru` updated in place, lines
    1185-1187, loop `depth++`), while the *refracted* ray is **pushed as a new
    stack frame** with `stk_dp = depth + 1` (lines 1174-1183).
  - `in_med` discriminated by `mid_c.x > 1.0f` (line 734).

## The likely divergence points (hypotheses, priority order)

1. **Depth accounting across a branched glass path.** CPU: one global `depth`,
   +1 on *each* recursive call (reflection and refraction both). GPU: the
   reflection consumes the current frame's `for`-loop depth budget; the
   refraction is a fresh frame at `dp0 = depth+1`. Verify a spawned ray's
   effective depth equals the CPU's `depth+1` at the equivalent point, and that
   the cutoff (`depth > MAX_DEPTH` vs `depth <= MAX_DEPTH`) shades the same set
   of bounces. A one-bounce mismatch changes energy on deep glass paths.
2. **Throughput / fresnel weighting.** CPU multiplies `refl_col`/`refr_col` by
   the per-channel fresnel/`f0mix` and `Tseg`/`sc`. GPU applies `stk_th`
   (throughput) = `cur_thru * wt * sc_col` (line 1177) with `wr`/`wt` the
   reflect/refract weights. Confirm `wr`/`wt` (and the iridescence per-channel
   blend) are bit-identical to the CPU's fresnel, and that `cur_thru = thru *
   Tseg` (line 1150) matches the CPU's `Tseg` placement.
3. **Medium state on push.** CPU `refr_med` (909-916) vs GPU `mid_rc`
   (1160-1172): front/back `side`/`side_entry` test, `vol_th > 0` gate, enter
   (medium = material) vs exit (air). A mismatch changes the in-medium
   attenuation and the next crossing's eta.
4. **Tseg / Beer-Lambert segment length.** Confirm the traversed in-medium
   segment length and attenuation (`mid_d`/`stk_ma`, `att_dist`) match the
   CPU's `Tseg` exactly (both the entry and exit crossings).

## Reproduction (always confirm the backend line)

```sh
make   # parity.sh does NOT build

# lamp (fast)
./ray2 --cpu test_scenes/scene_lamp_stdout.json > /tmp/lamp_cpu.ppm 2>/tmp/lamp_cpu.err
./ray2        test_scenes/scene_lamp_stdout.json > /tmp/lamp_gpu.ppm 2>/tmp/lamp_gpu.err
grep -a '^backend' /tmp/lamp_gpu.err            # MUST say "backend: metal"
python3 tools/ppm_diff.py /tmp/lamp_cpu.ppm /tmp/lamp_gpu.ppm
# expect: differing=27166 (3.45%) sum=167753 max=80

# dragon (slower, a few minutes)
./ray2 --cpu test_scenes/scene_dragon_stdout.json > /tmp/drg_cpu.ppm 2>/tmp/drg_cpu.err
./ray2        test_scenes/scene_dragon_stdout.json > /tmp/drg_gpu.ppm 2>/tmp/drg_gpu.err
grep -a '^backend' /tmp/drg_gpu.err
python3 tools/ppm_diff.py /tmp/drg_cpu.ppm /tmp/drg_gpu.ppm
# expect: differing=46688 (5.94%) sum=185981 max=53
```

A GPU run that silently fell back to CPU (`backend: cpu (metal failed: …)`)
makes a diff a false CPU-vs-CPU pass — always check the backend line.

## Investigation protocol

1. Reproduce both; confirm `backend: metal` on the GPU runs.
2. Localize the diff spatially: it should concentrate on/behind the glass
   (transmission), not the sky or opaque surfaces. Crop the diff to the glass
   region to see the pattern (edge vs whole-surface, one side vs both).
3. Build a minimal repro (like `/tmp/only_gold.json` was for envtest): a single
   glass sphere (ior 1.5, a volume with `vol_th` for the dragon case) + a known
   env, camera through it. Isolate which hypothesis (1-4) fires.
4. **Per-bounce trace diff** (the decisive step): for ONE pixel, log the bounce
   sequence on both backends — `(depth, hit_type, mat, eta, side, fresnel wr/wt,
   medium ior, Tseg, throughput)` — and diff the sequences. The first row that
   differs is the bug.
   - CPU: there is a `RAY_SPHERE_DBG` fprintf pattern at `renderer.cc:852`
     (currently an uncommitted TEMP-DBG) to extend.
   - GPU: `SceneGpu` has `dbg_x`/`dbg_y` (a single-pixel log hook); extend it to
     dump the per-bounce sequence to the debug buffer (buffer 11) and read it
     back on the host.
5. Confirm the root cause, then decide **which backend is physically correct**
   (check energy conservation / the reference renders) before fixing the other.
   Do not assume the CPU is right — AGENTS.md calls it the source of record,
   but verify for this specific term.

## Fix approach

- Fix the **wrong** backend to match the correct one, operation-for-operation
  (AGENTS.md "CPU/GPU mirror" — a shading-term change must land on both sides or
  parity breaks; here the goal is to make the two *already-mirrored* terms
  actually agree).
- If the fix is a structural change to the GPU's iterative traversal (e.g. depth
  accounting or the reflection-continues-vs-refraction-pushes split), keep it
  minimal and re-verify **all** scenes, not just lamp/dragon.
- Keep `SceneGpu`/struct sizes in sync with the `static_assert`s on both sides
  (currently `SceneGpu == 140`).

## Verification (four-row harness — AGENTS.md "Render parity")

This is a shading change, so the commit-delta rows (2-3) will move; that is
expected. For lamp and dragon:

1. new CPU vs new GPU ≤ new recorded baseline (per scene).
2. baseline GPU vs new GPU — moves (shading change); eyeball it is the glass
   region improving, not a new region breaking.
3. baseline CPU vs new CPU — moves only if the CPU side was edited.
4. baseline CPU vs baseline GPU = the old recorded signature.

Then:
- `tools/parity.sh --rebaseline test_scenes/scene_lamp_stdout.json`
- `tools/parity.sh --rebaseline test_scenes/scene_dragon_stdout.json`
  (writes `ok` only if max_channel_err ≤ 127; if it comes out `known-bug`, the
  fix made it *worse* — stop and re-examine.)
- Re-run the full no-arg gate `tools/parity.sh` and the other scenes (dish,
  suzanne, envtest) to confirm **no regressions**.

## Guardrails

- Do NOT "fix" a floor by rebaselining without a understood root cause.
- Do NOT regress the other four scenes; the full gate must stay green.
- `renderer.cc` has an uncommitted `RAY_SPHERE_DBG` TEMP-DBG block (line 852)
  and `gallery.py` has uncommitted changes — **do not commit those**; they are
  unrelated to this work.
- Commit only when asked; stage only the intended files.

## Open questions to resolve first

- Which backend is physically correct on the diverging glass paths? (Verify
  against energy conservation / reference before choosing which side to fix.)
- Is the divergence purely depth-accounting, or does it also involve the
  medium/attenuation model (hypotheses 3-4)?
- Does the dragon (a `KHR_materials_volume` with absorption) diverge for the
  same reason as the lamp (plain glass), or a different one (attenuation)?
