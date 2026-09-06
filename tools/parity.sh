#!/bin/bash
# CPU/GPU parity gate with a real backend check and per-scene baselines.
#
# Two different bars (see AGENTS.md "Render parity") — never conflate them:
#   - Commit delta (same backend, before vs after): must be 0.
#   - Cross-backend (CPU vs GPU): a per-scene floor, never zero. This gate
#     compares the current CPU-vs-GPU signature against the scene's recorded
#     baseline and FAILS on regression (any metric above baseline), not on
#     being nonzero.
#
# A scene is only gateable if it has a line in tools/parity_baselines.txt.
# Certification (--rebaseline) writes status "ok" only when BOTH
#   p99_9_channel_err <= FLOOR_P999   AND   n_severe <= FLOOR_SEVERE
# else "known-bug". Two complementary bars, both per-channel (0-255 scale,
# comparable to max_channel_err):
#   - p99_9_channel_err: nearest-rank 99.9th percentile of the per-channel
#     error over the channels that differ. Tail-robust: a few firefly channels
#     push max_channel_err high without moving it. Catches a broad moderate
#     divergence (a region wrong by a moderate amount).
#   - n_severe: count of channels with |error| >= 127 (near-full-range). Catches
#     a small HIGH-error region that the sparse-tail percentile is robust to.
# max_channel_err is still recorded and regression-gated, but no longer decides
# certification. A "known-bug" scene FAILS until fixed and re-baselined.
#
#   tools/parity.sh [SCENE ...]              # run the gate
#   tools/parity.sh --rebaseline SCENE ...   # record/refresh baselines
#
# For each scene: render CPU and GPU, verify from ray2's stderr "backend:" line
# that each run used the backend it claims, then diff with tools/ppm_diff.py. A
# GPU run that silently fell back to CPU would make the diff a CPU-vs-CPU false
# pass, so the gate refuses to report it: the diff is skipped and it exits
# non-zero.
#
# Defaults to the phase-2 parity set (iri dish 256 + envtest). Per-run PPMs and
# stderr logs land in a fresh /tmp/parity.XXXXXX dir, printed at the top.
#
# macOS /bin/bash 3.2 compatible.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUTDIR="$(mktemp -d /tmp/parity.XXXXXX)"
BASELINE_FILE="tools/parity_baselines.txt"
# FLOOR_P999: cert bar on p99_9_channel_err. Legit floors measured 2026-09-05
# top out at p99.9=20 (dragon), lamp 19. p999_B tracks a faulted region's max
# brightness (fault injection: 20x20 zeroed box reads ~15/19/24/34/42/62 for
# region max 15/20/25/35/45/75). A fault dimmer than the firefly floor (~20) is
# indistinguishable from fireflies, so the bar can only sit just above it: 22
# catches every measured fault (dimmest real = 24) with 2 units over the floor.
FLOOR_P999=22
# FLOOR_SEVERE: cert bar on n_severe (channels with |error| >= 127). Legit
# floors measured 2026-09-05 top out at n_severe=4 (dragon fireflies); all
# others 0. Fault injection: a fully-bright zeroed box adds ~3 severe channels
# per pixel (1x1->7, 2x2->16, 3x3->31). 8 = 2x the firefly floor: catches a
# 2x2+ high-error region while still allowing a single firefly-level pixel (7).
FLOOR_SEVERE=8

REBASELINE=0
SCENES=()
for a in "$@"; do
    if [ "$a" = "--rebaseline" ]; then
        REBASELINE=1
    else
        SCENES+=("$a")
    fi
done
if [ ${#SCENES[@]} -eq 0 ]; then
    if [ $REBASELINE -eq 1 ]; then
        echo "error: --rebaseline requires at least one SCENE" >&2
        exit 2
    fi
    SCENES=(test_scenes/scene_iri_dish_parity256.json
            test_scenes/scene_envtest_stdout.json)
fi
echo "parity gate: artifacts in $OUTDIR"

fail=0

run_render() { # $1=scene $2=tag $3=want (metal|cpu)
    scene=$1; tag=$2; want=$3
    if [ "$want" = cpu ]; then
        ./ray2 --cpu "$scene" > "$OUTDIR/$tag.ppm" 2> "$OUTDIR/$tag.err" || {
            echo "FAIL: ray2 exited nonzero for $scene" >&2; fail=1; return 1; }
    else
        ./ray2 "$scene" > "$OUTDIR/$tag.ppm" 2> "$OUTDIR/$tag.err" || {
            echo "FAIL: ray2 exited nonzero for $scene" >&2; fail=1; return 1; }
    fi
    if ! grep -q "^backend: $want\$" "$OUTDIR/$tag.err"; then
        echo "FAIL: $scene $want run did not use the $want backend — see $OUTDIR/$tag.err" >&2
        grep '^backend:' "$OUTDIR/$tag.err" >&2
        fail=1
        return 1
    fi
}

run_diff() { # $1=cpu.ppm $2=gpu.ppm -> sets DIFF_COUNT DIFF_SUM DIFF_MAX DIFF_P999 DIFF_SEVERE
    local out
    out="$(python3 tools/ppm_diff.py "$OUTDIR/$1" "$OUTDIR/$2")" || {
        echo "FAIL: ppm_diff failed for $1 vs $2" >&2; return 1; }
    echo "$out"
    DIFF_COUNT="${out#*differing=}"; DIFF_COUNT="${DIFF_COUNT%% *}"
    DIFF_SUM="${out#*sum_abs_err=}";   DIFF_SUM="${DIFF_SUM%% *}"
    DIFF_MAX="${out#*max_channel_err=}"; DIFF_MAX="${DIFF_MAX%% *}"
    DIFF_P999="${out#*p99_9_channel_err=}"; DIFF_P999="${DIFF_P999%% *}"
    DIFF_SEVERE="${out#*n_severe=}"; DIFF_SEVERE="${DIFF_SEVERE%% *}"
}

lookup_baseline() { # $1=scene -> "status differing sum max p999 severe" (empty if none)
    [ -f "$BASELINE_FILE" ] || return 0
    awk -F'\t' -v s="$1" '!/^\#/ && $2==s {print $1, $3, $4, $5, $6, $7}' "$BASELINE_FILE"
}

upsert_baseline() { # $1=scene $2=status $3=differing $4=sum $5=max $6=p999 $7=severe
    local scene=$1 status=$2 d=$3 s=$4 m=$5 p=$6 v=$7
    local tmp
    tmp="$(mktemp "${BASELINE_FILE}.XXXXXX")"
    awk -F'\t' -v OFS='\t' -v sc="$scene" -v st="$status" -v d="$d" -v s="$s" -v m="$m" -v p="$p" -v v="$v" '
        !/^\#/ { if ($2 == sc) { print st, sc, d, s, m, p, v; done=1; next } }
                { print }
        END { if (!done) print st, sc, d, s, m, p, v }
    ' "$BASELINE_FILE" > "$tmp"
    mv "$tmp" "$BASELINE_FILE"
}

gate_scene() { # $1=scene
    local scene=$1 base relscene ok bl st rest bdiff bsum bmax bp999 bsevere regress
    base="$(basename "$scene" .json)"
    echo "== $scene =="
    ok=1
    run_render "$scene" "${base}_cpu" cpu  || ok=0
    run_render "$scene" "${base}_gpu" metal || ok=0
    if [ "$ok" = 0 ]; then
        echo "SKIP diff: $base (backend guard failed — CPU-vs-CPU would be a false pass)"
        fail=1
        return
    fi
    run_diff "${base}_cpu.ppm" "${base}_gpu.ppm" || { fail=1; return; }
    relscene="${scene#./}"; relscene="${relscene#"$ROOT"/}"
    bl="$(lookup_baseline "$relscene")"
    if [ -z "$bl" ]; then
        echo "FAIL: $base — no baseline recorded (run: tools/parity.sh --rebaseline $scene)"
        fail=1
        return
    fi
    st="${bl%% *}"; rest="${bl#* }"
    bdiff="${rest%% *}"; rest="${rest#* }"
    bsum="${rest%% *}"; rest="${rest#* }"
    bmax="${rest%% *}"; rest="${rest#* }"
    bp999="${rest%% *}"; rest="${rest#* }"
    bsevere="${rest%% *}"
    if [ "$st" = "known-bug" ]; then
        echo "FAIL: $base — known CPU/GPU divergence (baseline p99_9=$bp999 n_severe=$bsevere); fix it, then re-baseline"
        fail=1
        return
    fi
    regress=0
    [ "$DIFF_COUNT"  -gt "$bdiff"   ] && regress=1
    [ "$DIFF_SUM"    -gt "$bsum"    ] && regress=1
    [ "$DIFF_MAX"    -gt "$bmax"    ] && regress=1
    [ "$DIFF_P999"   -gt "$bp999"   ] && regress=1
    [ "$DIFF_SEVERE" -gt "$bsevere" ] && regress=1
    if [ "$regress" = 1 ]; then
        echo "FAIL: $base — regression vs baseline (cur diff=$DIFF_COUNT sum=$DIFF_SUM max=$DIFF_MAX p999=$DIFF_P999 severe=$DIFF_SEVERE  >  base diff=$bdiff sum=$bsum max=$bmax p999=$bp999 severe=$bsevere)"
        fail=1
        return
    fi
    echo "PASS: $base (diff=$DIFF_COUNT sum=$DIFF_SUM max=$DIFF_MAX p999=$DIFF_P999 severe=$DIFF_SEVERE  <=  base diff=$bdiff sum=$bsum max=$bmax p999=$bp999 severe=$bsevere)"
}

rebaseline_scene() { # $1=scene
    local scene=$1 base relscene ok st
    base="$(basename "$scene" .json)"
    echo "== rebaseline $scene =="
    ok=1
    run_render "$scene" "${base}_cpu" cpu  || ok=0
    run_render "$scene" "${base}_gpu" metal || ok=0
    if [ "$ok" = 0 ]; then
        echo "SKIP: $base (backend guard failed — not baselined)"
        fail=1
        return
    fi
    run_diff "${base}_cpu.ppm" "${base}_gpu.ppm" || { fail=1; return; }
    if [ "$DIFF_P999" -le "$FLOOR_P999" ] && [ "$DIFF_SEVERE" -le "$FLOOR_SEVERE" ]; then
        st="ok"
    else
        st="known-bug"
        echo "WARNING: $base p99_9=$DIFF_P999 (floor $FLOOR_P999) n_severe=$DIFF_SEVERE (floor $FLOOR_SEVERE) — real CPU/GPU divergence, recorded as known-bug (gate will FAIL until fixed)"
    fi
    relscene="${scene#./}"; relscene="${relscene#"$ROOT"/}"
    upsert_baseline "$relscene" "$st" "$DIFF_COUNT" "$DIFF_SUM" "$DIFF_MAX" "$DIFF_P999" "$DIFF_SEVERE"
    echo "recorded: $st  $relscene  differing=$DIFF_COUNT sum_abs_err=$DIFF_SUM max_channel_err=$DIFF_MAX p99_9_channel_err=$DIFF_P999 n_severe=$DIFF_SEVERE"
}

if [ $REBASELINE -eq 1 ]; then
    for scene in "${SCENES[@]}"; do
        rebaseline_scene "$scene"
    done
    if [ "$fail" = 0 ]; then
        echo "REBASELINE: OK — updated $BASELINE_FILE (artifacts in $OUTDIR)"
    else
        echo "REBASELINE: some scenes failed (artifacts in $OUTDIR)"
    fi
else
    for scene in "${SCENES[@]}"; do
        gate_scene "$scene"
    done
    if [ "$fail" = 0 ]; then
        echo "PARITY GATE: PASS (artifacts in $OUTDIR)"
    else
        echo "PARITY GATE: FAIL (artifacts in $OUTDIR)"
    fi
fi
exit "$fail"
