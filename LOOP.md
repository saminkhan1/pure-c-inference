# Voxtral Autoresearch Session

*Inspired by karpathy/autoresearch, uditgoenka/autoresearch, and davebcn87/pi-autoresearch.*

## Goal

Maximize `composite_score` for Mac dictation use case on Apple Silicon (M4 Max).

**Metric:** `composite_score` (0..1, higher is better)  
**Target:** composite_score > 0.90  
**Baseline:** run `./autoresearch.sh --set-baseline` once on a clean commit to record starting point

## How to Run One Experiment

```bash
# 1. Make a focused change to main.c
# 2. Commit before benchmarking (git is memory)
git add main.c && git commit -m "experiment: <one-line description>"

# 3. Run the benchmark (~15 min, all files)
./autoresearch.sh

# 4. Decision:
#    If METRIC composite_score > baseline + 0.002 → keep it
#    Else → git revert --no-edit HEAD
```

## Loop Protocol (8 Rules)

1. **Loop until interrupted** — repeat forever (or N iterations). Never stop on a single failure.
2. **Read before write** — read `git log --oneline -20` and `git diff HEAD~1` before each iteration.
3. **One change per iteration** — atomic changes only. If something breaks, you know exactly why.
4. **Mechanical verification only** — `composite_score` is the single judge. No subjective calls.
5. **Auto-rollback** — if score doesn't improve by ≥0.002: `git revert --no-edit HEAD`.
6. **Simplicity wins** — equal score + less code = KEEP. Complexity has hidden latency costs.
7. **Git is memory** — use `experiment:` commit prefix. Read `git log` before each iteration. Failed experiments stay in history; learn from the pattern.
8. **When stuck, think harder** — re-read `main.c`, combine near-misses from git history, try radical changes. Don't repeat failed directions.

## Files the Agent Can Modify

| File | Notes |
|------|-------|
| `main.c` | Only file for inference optimizations. Metal, KV cache, decode loop all here. |
| `Makefile` | Compiler flags only (e.g. `-O3 -ffast-math -march=native`). No restructuring. |

## Files the Agent Must NOT Modify (Frozen)

| File | Why Frozen |
|------|------------|
| `eval_harness.py` | Defines the metric — modifying it voids all results |
| `prepare_datasets.py` | Defines the eval dataset |
| `autoresearch.sh` | Defines the gates — anti-cheat provision |
| `samples/benchmark/` | The eval data itself |
| `AUTORESEARCH.md` | Benchmark spec |

`autoresearch.sh` verifies these files are unmodified before running. If the agent changes them, the benchmark exits with an error.

## Improvement Threshold

Min delta to KEEP: **+0.002** (noise floor ~0.003 at 120 files)

## Composite Score Breakdown

```
Quality    40%  =  1 - avg_WER                        (LibriSpeech + dictation_micro)
Latency    25%  =  TTFT/500ms + EP_latency/300ms       (normalized, inference-only)
Stability  20%  =  1.0 (offline placeholder)
Efficiency 10%  =  1 - inference_rtf                  (target RTF < 0.50 on M4)
Robustness  5%  =  success rate
```

Key insight: `inference_rtf = ttf_ms / (audio_sec * 1000)` — excludes model loading overhead.  
`json_time_start_ms` in `main.c:805` is set AFTER `vox_load()`, so JSON_METRICS are inference-only.

## Patch Classes

One patch class per iteration to prevent thrashing:

1. **Systems Hotspots** — memory layout, buffer reuse, KV cache writes, Metal command-buffer batching
2. **Compiler/Build** — `-O3`, `-ffast-math`, `-march=native`, LTO, SIMD flags
3. **Numerical Parity** — spot-check against `python_simple_implementation.py`
4. **Streaming-Policy** — chunk size, decode loop structure (less impact on offline files)

## Architecture Reference

- `vox_load()` precedes `json_time_start_ms` — JSON_METRICS are pure inference time
- Encoder: streaming KV cache, window=750, rolling compaction
- Decoder: KV cache window=8192
- MPS backend: Metal shader cache cold on first run → warm via smoke test in `autoresearch.sh`
- Offline files: `-I` interval flag has negligible effect on batch inference

## Experiment History

<!-- Agent: append one line per KEPT experiment. Format: date | commit | delta | description -->

| Date | Commit | Delta | Description |
|------|--------|-------|-------------|

