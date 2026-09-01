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
# Certification (--rebaseline) writes status "ok" only when max_channel_err is
# <= FLOOR_MAX, else "known-bug". FLOOR_MAX is the "no near-full-range pixel"
# bar: a legitimate floor never puts a pixel at >=128/255 in any channel, but a
# real structural bug (one backend black where the other is bright) does. A
# "known-bug" scene FAILS until its divergence is fixed and it is re-baselined.
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
# A pixel at > FLOOR_MAX (i.e. >=128) in any channel is a near-full-range
# divergence, not float rounding. Legit floors measured 2026-08-31 top out at
# max_channel_err=80 (lamp) with ZERO pixels at err>=128; the envtest bug has
# 2659 (13.6%). 127 = "strictly under half the 8-bit range".
FLOOR_MAX=127

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

run_diff() { # $1=cpu.ppm $2=gpu.ppm -> sets DIFF_COUNT DIFF_SUM DIFF_MAX
    local out
    out="$(python3 tools/ppm_diff.py "$OUTDIR/$1" "$OUTDIR/$2")" || {
        echo "FAIL: ppm_diff failed for $1 vs $2" >&2; return 1; }
    echo "$out"
    DIFF_COUNT="${out#*differing=}"; DIFF_COUNT="${DIFF_COUNT%% *}"
    DIFF_SUM="${out#*sum_abs_err=}";   DIFF_SUM="${DIFF_SUM%% *}"
    DIFF_MAX="${out#*max_channel_err=}"; DIFF_MAX="${DIFF_MAX%% *}"
}

lookup_baseline() { # $1=scene -> "status differing sum max" (empty if none)
    [ -f "$BASELINE_FILE" ] || return 0
    awk -F'\t' -v s="$1" '!/^\#/ && $2==s {print $1, $3, $4, $5}' "$BASELINE_FILE"
}

upsert_baseline() { # $1=scene $2=status $3=differing $4=sum $5=max
    local scene=$1 status=$2 d=$3 s=$4 m=$5
    local tmp
    tmp="$(mktemp "${BASELINE_FILE}.XXXXXX")"
    awk -F'\t' -v OFS='\t' -v sc="$scene" -v st="$status" -v d="$d" -v s="$s" -v m="$m" '
        !/^\#/ { if ($2 == sc) { print st, sc, d, s, m; done=1; next } }
                { print }
        END { if (!done) print st, sc, d, s, m }
    ' "$BASELINE_FILE" > "$tmp"
    mv "$tmp" "$BASELINE_FILE"
}

gate_scene() { # $1=scene
    local scene=$1 base relscene ok bl st rest bdiff bsum bmax regress
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
    bsum="${rest%% *}"; bmax="${rest#* }"
    if [ "$st" = "known-bug" ]; then
        echo "FAIL: $base — known CPU/GPU divergence (baseline max_channel_err=$bmax > FLOOR_MAX=$FLOOR_MAX); fix it, then re-baseline"
        fail=1
        return
    fi
    regress=0
    [ "$DIFF_COUNT" -gt "$bdiff" ] && regress=1
    [ "$DIFF_SUM"   -gt "$bsum"   ] && regress=1
    [ "$DIFF_MAX"   -gt "$bmax"   ] && regress=1
    if [ "$regress" = 1 ]; then
        echo "FAIL: $base — regression vs baseline (cur diff=$DIFF_COUNT sum=$DIFF_SUM max=$DIFF_MAX  >  base diff=$bdiff sum=$bsum max=$bmax)"
        fail=1
        return
    fi
    echo "PASS: $base (diff=$DIFF_COUNT sum=$DIFF_SUM max=$DIFF_MAX  <=  base diff=$bdiff sum=$bsum max=$bmax)"
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
    if [ "$DIFF_MAX" -le "$FLOOR_MAX" ]; then
        st="ok"
    else
        st="known-bug"
        echo "WARNING: $base max_channel_err=$DIFF_MAX > FLOOR_MAX=$FLOOR_MAX — real CPU/GPU divergence, recorded as known-bug (gate will FAIL until fixed)"
    fi
    relscene="${scene#./}"; relscene="${relscene#"$ROOT"/}"
    upsert_baseline "$relscene" "$st" "$DIFF_COUNT" "$DIFF_SUM" "$DIFF_MAX"
    echo "recorded: $st  $relscene  differing=$DIFF_COUNT sum_abs_err=$DIFF_SUM max_channel_err=$DIFF_MAX"
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
