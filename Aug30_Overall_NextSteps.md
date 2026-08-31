> **Superseded:** see `Aug31_State.md` for the current state, reconstructed and verified against HEAD `d31b10c` (2026-08-31).

Confirmed: TriGpu is unchanged from the plan document (t0/t1/t2 + mesh_idx, i.e., 100 bytes). The plan is accurate against HEAD.

Summary of situation:

Current location (on branch phase2-iridescence-wip, tree is clean):
- CPU + Metal renderer. With strict parity harness (4-line gates in AGENTS.md).
- Phase 2 (IBL / iridescence / envmap work) closed: commit ace5b97 "Close Phase 2 IBL", lamp scene is 3.45% cross-backend (the "byte-identical" reading was a CPU-vs-CPU false positive from the Metal page fault), dish scene rebaselined to a 0.23% floor.
- ao_tex_index plumbing completed (74acd37) — the data path exists, but nothing in the shading reads it yet.
- Phase 3 plan was created at HEAD (d31b10c), "planned, not started."
- There is leftover TEMP-DBG env-escape instrumentation code gated behind RAY_GDEBUG (intentionally committed, no effect on rendering).

Next step following phase3_plan.md §8, one commit per step:
1. TriGpu expansion (100 → 148 bytes, tan0/tan1/tan2 with w handedness) + vendor MikkTSpace, run before the world-space bake, update all ~54 TriGpu build sites, no reads from shading. Gate: zero-pixel commit diff across all 6 test scenes.
2. Normal mapping shading (CPU + GPU, linear sampling, normalTexture.scale — glassCover is 2, so test scale=2 on glassCover).
3. AO shading (RG is ao_tex_index, fallback to R of orm_tex_index, apply to ambient + per-light diffuse, specular stays without AO).
4. MASK — requires a prior judgment call on §5's alpha sampling (hand-written bilinear on GPU is recommended, to preserve byte identity).

Potential pitfalls flagged: fast-math is ON on Metal (TBN stability, ε=2/255 upper bound), the layout audit before shader code, the single-commit synchronization rule for struct layout, and verification that the "GPU unrunnable" gate 2.4 issue has been cleared (commit ace5b97 records "CPU passed, GPU unrunnable" for the old gate — this needs to be reconfirmed).
Where we are (branch phase2-iridescence-wip, clean tree, HEAD d31b10c):
- Dual-backend CPU+Metal renderer with a strict parity culture: four-row commit-delta gate (0% byte-identical) vs cross-backend gate (≤ recorded baseline, currently 137px/0.23% on the dish scene).
- Phase 2 (IBL/iridescence) just closed. Lamp scene is now 3.45% cross-backend (the "byte-identical" reading was a CPU-vs-CPU false positive from the Metal page fault); dish was rebaselined.
- ao_tex_index is plumbed end-to-end (structs, parser, GPU upload) but nothing reads it yet.
- Residue: TEMP-DBG env-escape instrumentation still in the tree (shaders.metal:79, gpu_renderer.mm:440+), gated behind RAY_GDEBUG. Harmless, but worth retiring before Phase 3 pollutes the diff harness.
- phase3_plan.md (written at HEAD, verified accurate): normal maps + AO + MASK, all "planned, not started." I spot-checked its facts — TriGpu is still 100 bytes, layout as described.
Next steps, per the plan's own sequencing:
1. Pre-work: make the plan's "GPU not runnable" note in ace5b97 moot — confirm the Metal path actually runs on this machine (it writes dbg_x unconditionally at gpu_renderer.mm:617-620, worth checking that compiles/runs clean), then decide whether to strip TEMP-DBG as its own commit.
2. Step 1: TriGpu expansion (100→148, tan0/1/2 with handedness) + vendored MikkTSpace, wired pre-world-bake, all ~54 construction sites, tangents populated but unread. Gate: 0px commit-delta on all six test scenes.
3. Step 2: normal-mapping shading (linear texture sampling, normalTexture.scale — glassCover uses 2).
4. Step 3: AO (fallback chain ao_tex_index→orm_tex_index, ambient+diffuse only).
5. Step 4: MASK — but §5 needs a decision first: recommended option 1 (hand-rolled bilinear alpha on GPU to preserve byte-identity).
