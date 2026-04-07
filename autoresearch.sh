#!/usr/bin/env bash
# autoresearch.sh — voxtral.c benchmark script
#
# Usage:
#   ./autoresearch.sh                # run full eval (~15 min, all files)
#   ./autoresearch.sh --set-baseline # run full eval and save score as baseline
#
# Output (stdout, grep-able by agent):
#   METRIC composite_score=0.8472
#   METRIC delta=+0.0031              (vs baseline, if baseline exists)
#
# Exit codes:
#   0 = eval complete
#   1 = build failed
#   2 = smoke test failed
#   3 = eval harness failed

set -euo pipefail
cd "$(dirname "$0")"

BINARY=./voxtral
MODEL=voxtral-model
PYTHON=./venv/bin/python
EVAL=eval_harness.py
BASELINE_FILE=bench/baseline_score.txt
HISTORY_FILE=bench/autoresearch_history.jsonl
OUTPUT_FILE=bench/eval_latest.json

SAVE_BASELINE=0
for arg in "$@"; do
    case "$arg" in
        --set-baseline) SAVE_BASELINE=1 ;;
    esac
done

# ── Anti-cheat ────────────────────────────────────────────────────────────────
# The eval infrastructure is frozen. The agent must NOT modify these files.
# Changes here would let the agent game composite_score without improving voxtral.
FROZEN_FILES="$EVAL prepare_datasets.py autoresearch.sh"
DIRTY=$(git diff --name-only -- $FROZEN_FILES 2>/dev/null || true)
if [ -n "$DIRTY" ]; then
    echo "ERROR: frozen eval infrastructure modified. Revert before benchmarking."
    echo "Modified files: $DIRTY"
    echo "These files define the metric — modifying them voids the result."
    exit 1
fi

COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
echo "commit=$COMMIT"

# ── Tier 1: Build ─────────────────────────────────────────────────────────────
echo ""
echo "=== Tier 1: Build ==="
if ! make mps 2>&1; then
    echo "BUILD FAILED"
    exit 1
fi
echo "Build OK"

# ── Tier 2: Smoke test ────────────────────────────────────────────────────────
# Verifies the binary produces non-empty output on the reference audio.
echo ""
echo "=== Tier 2: Smoke test ==="
SMOKE_OUT=$("$BINARY" -d "$MODEL" -i test_speech.wav --silent 2>/dev/null || true)
if [ -z "$SMOKE_OUT" ]; then
    echo "SMOKE FAILED: no output from test_speech.wav"
    exit 2
fi
WORD_COUNT=$(echo "$SMOKE_OUT" | wc -w | tr -d ' ')
if [ "$WORD_COUNT" -lt 3 ]; then
    echo "SMOKE FAILED: suspiciously short output ($WORD_COUNT words): $SMOKE_OUT"
    exit 2
fi
echo "Smoke OK: $SMOKE_OUT"

# ── Tier 3: Full eval ─────────────────────────────────────────────────────────
echo ""
echo "=== Tier 3: Full eval ==="
if ! "$PYTHON" "$EVAL" --output "$OUTPUT_FILE"; then
    echo "EVAL HARNESS FAILED"
    exit 3
fi

# ── Extract composite score ───────────────────────────────────────────────────
SCORE=$("$PYTHON" -c \
    "import json; d=json.load(open('$OUTPUT_FILE')); print(f'{d[\"composite_score\"]:.4f}')" \
    2>/dev/null || echo "")
if [ -z "$SCORE" ]; then
    echo "ERROR: could not parse composite_score from $OUTPUT_FILE"
    exit 3
fi

# ── Baseline comparison ───────────────────────────────────────────────────────
if [ "$SAVE_BASELINE" = "1" ]; then
    mkdir -p bench
    echo "$SCORE" > "$BASELINE_FILE"
    echo "Baseline saved: $SCORE"
fi

DELTA_LINE=""
if [ -f "$BASELINE_FILE" ]; then
    BASELINE=$(cat "$BASELINE_FILE")
    DELTA=$("$PYTHON" -c \
        "b=float('$BASELINE'); s=float('$SCORE'); d=s-b; print(f'{d:+.4f} ({\"IMPROVE\" if d>0.002 else \"REGRESS\" if d<-0.001 else \"NOISE\"}'+')')" \
        2>/dev/null || echo "?")
    DELTA_LINE="METRIC delta=$DELTA (baseline=$BASELINE)"
fi

# Append summary to persistent history (one JSON per line, never overwritten)
mkdir -p bench
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
"$PYTHON" -c "
import json, os
entry = {
    'timestamp': '$TIMESTAMP',
    'commit': '$COMMIT',
    'composite_score': float('$SCORE'),
    'baseline': float(open('$BASELINE_FILE').read().strip()) if os.path.exists('$BASELINE_FILE') else None
}
with open('$HISTORY_FILE', 'a') as f:
    f.write(json.dumps(entry) + '\n')
" 2>/dev/null || true

# ── Machine-readable output ───────────────────────────────────────────────────
echo ""
echo "METRIC composite_score=$SCORE"
[ -n "$DELTA_LINE" ] && echo "$DELTA_LINE"
