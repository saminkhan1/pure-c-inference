# Voxtral Autoresearch Spec: Mac Dictation & Realtime Performance

This document defines the autoresearch loop, benchmark metrics, and experiment queue for adapting `voxtral.c` into a dictation-grade engine capable of replacing Superwhisper on Mac and later iPhone.

## 1. Benchmark Stack (Four Layers)

To ensure legibility with Voxtral Realtime reporting and to capture dictation UX, evaluate at fixed delays (240 ms, 480 ms, 960 ms, 2400 ms) across four layers:

| Layer | Objective | Metrics |
|---|---|---|
| **Reference Parity** | Verify baseline correctness against trusted runs. | Token/logit agreement, transcript agreement, crash-free decoding rate. |
| **Standard ASR** | Maintain quality floor on industry datasets. | WER/CER on LibriSpeech, GigaSpeech, VoxPopuli, Common Voice (short/long/multilingual splits). |
| **Realtime Systems** | Quantify streaming and throughput efficiency. | Time-to-first-token (TTFT), median/p95 token latency, finalization latency, real-time factor (RTF), memory usage, CPU/GPU utilization, multi-stream throughput (1-16 streams). |
| **Dictation UX** | Measure the perceived feel of live typing on Mac. | Partial stability, backtrack rate, commit latency, endpoint latency (after speech stop), punctuation lag, correction burden. |

## 2. Metrics & Composite Scoreboard

For streaming and dictation, offline batch accuracy (WER) is only a quality floor. Optimization is guided by a weighted composite score reflecting the latency-quality tradeoff of daily dictation:

- **Quality (40%)**: WER/CER, Transcript Agreement.
- **Latency (25%)**: TTFT, Time-to-final, Endpoint latency, Commit latency.
- **Stability (20%)**: Time-to-stable-prefix, Prefix churn/backtrack rate.
- **Efficiency (10%)**: RTF, Peak Memory, Compute utilization.
- **Robustness (5%)**: Crash rate, Audio format/noise resilience.

## 3. Harness Design & JSON Schema

The benchmark harness must be strictly frozen before tuning (fixed datasets, hardware, delays). 
Every run emits one JSON row per utterance and a summary per suite. 

**Rule:** No benchmark result without full metadata.

*Example JSON Row:*
```json
{
  "commit_sha": "a1b2c3d",
  "compiler_flags": "-O3 -ffast-math",
  "backend": "metal",
  "model_hash": "...",
  "hardware": "Apple M3 Max",
  "requested_delay_ms": 480,
  "observed_latency_ms": 492,
  "time_to_first_token_ms": 120,
  "time_to_final_ms": 850,
  "prefix_churn_rate": 0.05,
  "transcript": "hello world",
  "reference_text": "Hello world.",
  "failure_mode": null
}
```

## 4. Patch Classes (Search Space)

To prevent thrashing, the search space is bounded into four families. Patches should only mutate one family at a time:

1. **Systems Hotspots**: Memory layout, buffer reuse, cache locality, fused operations, KV-cache writes, Metal command-buffer structure, and CPU/GPU overlap.
2. **Streaming-Policy Knobs**: Delay scheduling, chunk size, commit frequency, silence handling, partial-to-final commit rules.
3. **Numerical & Implementation Parity**: Verification against Python reference (`python_simple_implementation.py`) and public model behavior.
4. **Product-Behavior Tuning**: Mac-first workflow evaluations for dictation UX (typing responsiveness, command parsing).

## 5. Autoresearch Loop

A disciplined, gated loop for evaluating and promoting changes:

1. **Generate**: Agent creates a narrowly scoped patch targeting *one* patch class.
2. **Smoke Test**: Run correctness and crash checks (`make test`, basic audio runs).
3. **Cheap Benchmark Subset**: Run on "Mac dictation UX" and "480 ms public benchmark slice".
4. **Gate**: Promote the patch *only* if it beats the baseline by a minimum effect size on latency, stability, or memory *without* harming transcript quality.
5. **Full Suite**: Run full benchmark suite before merge.

## 6. Mac-First Experiment Queue

Execute the first autoresearch campaign in the following order:

| Priority | Focus Area | Goal |
|---|---|---|
| **1** | **Eval Harness** | Build the frozen harness (TTFT, time-to-stable-prefix, finalization latency, memory, crashes at 240/480/960ms delays). |
| **2** | **Decoder & GPU Scheduling** | Optimize the monolithic GPU decoder path (KV cache, attention on GPU) for wall-clock wins. |
| **3** | **Chunk / Commit Policies** | Tune incremental audio ingestion, chunk sizes, and commit events to improve perceived dictation responsiveness. |
| **4** | **Cross-stage Fusion** | Explore broader code edits (cross-stage fusion, deep memory refactors) safely once the pipeline boundaries are optimized. |