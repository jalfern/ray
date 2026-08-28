#!/bin/bash
# CPU/GPU parity gate with a real backend check.
#
# For each scene: render CPU and GPU, verify from ray2's stderr "backend:"
# line that each run used the backend it claims, then diff the images with
# tools/ppm_diff.py. A GPU run that silently fell back to CPU would make the
# diff a CPU-vs-CPU false pass, so the gate refuses to report it: the diff
# is skipped and the gate exits non-zero.
#
#   tools/parity.sh [SCENE ...]
#
# Defaults to the phase-2 parity set (iri dish 256 + envtest). Per-run PPMs
# and stderr logs land in a fresh /tmp/parity.XXXXXX dir, printed at the
# top. Exit 0 only if every run used its claimed backend AND every pair
# diffs to zero.
#
# macOS /bin/bash 3.2 compatible.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUTDIR="$(mktemp -d /tmp/parity.XXXXXX)"
SCENES=("$@")
if [ ${#SCENES[@]} -eq 0 ]; then
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

diff_pair() { # $1=cpu.ppm $2=gpu.ppm
    out="$(python3 tools/ppm_diff.py "$OUTDIR/$1" "$OUTDIR/$2")"
    echo "$out"
    case "$out" in
        *"differing=0 "*) return 0 ;;
        *) return 1 ;;
    esac
}

for scene in "${SCENES[@]}"; do
    base="$(basename "$scene" .json)"
    echo "== $scene =="
    ok=1
    run_render "$scene" "${base}_cpu" cpu  || ok=0
    run_render "$scene" "${base}_gpu" metal || ok=0
    if [ "$ok" = 1 ] && diff_pair "${base}_cpu.ppm" "${base}_gpu.ppm"; then
        echo "PASS: $base"
    else
        [ "$ok" = 0 ] && echo "SKIP diff: $base (backend guard failed — CPU-vs-CPU would be a false pass)"
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "PARITY GATE: PASS (artifacts in $OUTDIR)"
else
    echo "PARITY GATE: FAIL (artifacts in $OUTDIR)"
fi
exit "$fail"
