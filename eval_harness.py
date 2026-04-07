#!/usr/bin/env python3
"""
Autoresearch Evaluation Harness
Evaluates transcript quality (WER/CER) and latency (TTFT, inference RTF, endpoint latency)
across fixed delays to produce a composite scoreboard for autoresearch decisions.

Design principles:
- JSON_METRICS from the binary are inference-only (model already loaded) — use them as the
  primary latency signal, not wall_sec which includes model loading overhead.
- inference_rtf = ttf_ms / (audio_sec * 1000) strips model-load noise from RTF.
- Every result row carries commit_sha + hardware for cross-run traceability.
- A single composite_score drives autoresearch promotion decisions.
- p95 stats expose tail-latency regressions that mean-only metrics hide.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from dataclasses import dataclass, asdict, field
import jiwer
import string
import statistics

JSON_RE = re.compile(r"JSON_METRICS:\s+(.+)")
AUDIO_RE = re.compile(r"Audio:\s+\d+\s+samples\s+\(([0-9.]+)\s+seconds\)")
DECODER_RE = re.compile(
    r"Decoder:\s+\d+\s+text tokens\s+\((\d+)\s+steps\)\s+in\s+(\d+)\s+ms\s+"
    r"\(prefill\s+(\d+)\s+ms\s+\+\s+([0-9.]+)\s+ms/step\)"
)
GPU_RE = re.compile(r"Metal GPU:\s+([0-9.]+)\s+MB")


def get_commit_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return "unknown"


def get_hardware() -> str:
    """Returns a short hardware identifier, e.g. 'Apple M4 Max'."""
    try:
        out = subprocess.check_output(
            ["sysctl", "-n", "machdep.cpu.brand_string"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        if out:
            return out
    except Exception:
        pass
    try:
        out = subprocess.check_output(
            ["system_profiler", "SPHardwareDataType"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        for line in out.splitlines():
            if "Chip" in line or "Processor" in line:
                return line.split(":")[-1].strip()
    except Exception:
        pass
    return "unknown"


@dataclass
class EvalResult:
    audio_filepath: str
    dataset: str
    split: str
    delay_ms: int
    reference_text: str
    hypothesis_text: str
    # Quality
    wer: float
    cer: float
    # Latency — inference-only (json_time_start_ms is set after model load in main.c)
    ttft_ms: float  # time from feed start to first output token
    ttf_ms: float  # time from feed start to final token
    endpoint_latency_ms: float  # ttf_ms - audio_sec*1000 (how long after speech ends)
    inference_rtf: float  # ttf_ms / (audio_sec*1000) — true inference speed ratio
    # Wall clock (includes model load — use only for total runtime tracking)
    wall_sec: float
    # System
    audio_sec: float
    step_ms: float
    gpu_mb: float
    # Application-level (pipeline bottlenecks)
    encoder_ms: float
    decoder_ms: float
    n_restarts: int
    # Traceability
    commit_sha: str = ""
    hardware: str = ""
    success: bool = True
    error: str = ""


def clean_text(text: str) -> str:
    """Normalizes text for standard ASR scoring (uppercase, no punctuation)."""
    if not text:
        return ""
    text = text.upper()
    text = text.translate(str.maketrans("", "", string.punctuation))
    return " ".join(text.split())


def run_voxtral(binary: str, model_dir: str, wav_path: str, delay_ms: int) -> dict:
    """
    Run voxtral on a single file. Returns a dict of parsed metrics.
    Note: JSON_METRICS times are measured from after model load (main.c:805),
    so ttft_ms and ttf_ms are pure inference latencies.
    """
    cmd = [binary, "-d", model_dir, "-i", wav_path, "--json-metrics"]
    if delay_ms > 0:
        cmd.extend(["-I", str(delay_ms / 1000.0)])

    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        wall_sec = time.time() - t0

        if proc.returncode != 0:
            return {
                "success": False,
                "error": f"Exit {proc.returncode}: {proc.stderr[-200:]}",
                "wall_sec": wall_sec,
            }

        stdout_text = proc.stdout.strip()
        stderr_text = proc.stderr

        json_metrics = {}
        audio_sec = step_ms = gpu_mb = 0.0

        for line in stderr_text.splitlines():
            m = JSON_RE.search(line)
            if m:
                try:
                    json_metrics = json.loads(m.group(1))
                except json.JSONDecodeError:
                    pass
            if am := AUDIO_RE.search(line):
                audio_sec = float(am.group(1))
            if dm := DECODER_RE.search(line):
                step_ms = float(dm.group(4))
            if gm := GPU_RE.search(line):
                gpu_mb = float(gm.group(1))

        ttft_ms = json_metrics.get("time_to_first_token_ms", 0.0)
        ttf_ms = json_metrics.get("time_to_final_ms", 0.0)
        encoder_ms = json_metrics.get("encoder_ms", 0.0)
        decoder_ms = json_metrics.get("decoder_ms", 0.0)
        n_restarts = json_metrics.get("n_restarts", 0)

        # inference_rtf: pure inference time vs audio duration.
        # This is the signal to optimize — it does NOT include model loading.
        inference_rtf = (
            (ttf_ms / (audio_sec * 1000.0)) if audio_sec > 0 and ttf_ms > 0 else 0.0
        )

        # endpoint_latency: how long after the end of speech until final token.
        # Negative means decoding finished before the audio ended (prefill batch overlap).
        endpoint_latency_ms = ttf_ms - (audio_sec * 1000.0) if ttf_ms > 0 else 0.0

        return {
            "success": True,
            "stdout": stdout_text,
            "audio_sec": audio_sec,
            "step_ms": step_ms,
            "gpu_mb": gpu_mb,
            "ttft_ms": ttft_ms,
            "ttf_ms": ttf_ms,
            "encoder_ms": encoder_ms,
            "decoder_ms": decoder_ms,
            "n_restarts": n_restarts,
            "inference_rtf": inference_rtf,
            "endpoint_latency_ms": endpoint_latency_ms,
            "wall_sec": wall_sec,
            "error": "",
        }

    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": "Timeout after 120s",
            "wall_sec": time.time() - t0,
        }
    except Exception as e:
        return {"success": False, "error": str(e), "wall_sec": time.time() - t0}


def compute_composite_score(results: list[EvalResult]) -> float:
    """
    Single score for autoresearch promotion gating.

    Weights from AUTORESEARCH.md:
      Quality    40%  — (1 - avg_wer) on success runs
      Latency    25%  — normalized score on TTFT + endpoint_latency
      Stability  20%  — placeholder (no streaming stability signal from offline files)
      Efficiency 10%  — inference_rtf score (lower RTF = better)
      Robustness  5%  — success rate

    Returns 0..1 (higher is better).
    """
    success = [r for r in results if r.success]
    if not results:
        return 0.0

    # Quality (40%)
    avg_wer = _mean([r.wer for r in success]) if success else 1.0
    quality = max(0.0, 1.0 - avg_wer)

    # Latency (25%) — normalize TTFT against 500ms target, EP latency against 300ms target
    # Both are in ms; clamped to [0,1]
    ttft_score = (
        max(0.0, 1.0 - (_mean([r.ttft_ms for r in success if r.ttft_ms > 0]) / 500.0))
        if success
        else 0.0
    )
    ep_score = (
        max(
            0.0,
            1.0
            - (
                _mean(
                    [
                        r.endpoint_latency_ms
                        for r in success
                        if r.endpoint_latency_ms > 0
                    ]
                )
                / 300.0
            ),
        )
        if success
        else 0.0
    )
    latency = (ttft_score + ep_score) / 2.0

    # Stability (20%) — not measurable from offline files, credit full score
    stability = 1.0

    # Efficiency (10%) — inference RTF target <0.5 on M4
    avg_rtf = (
        _mean([r.inference_rtf for r in success if r.inference_rtf > 0])
        if success
        else 2.0
    )
    efficiency = max(0.0, 1.0 - avg_rtf / 1.0)  # 0 RTF=1.0 score, RTF>=1.0 score=0

    # Robustness (5%)
    robustness = len(success) / len(results)

    score = (
        0.40 * quality
        + 0.25 * latency
        + 0.20 * stability
        + 0.10 * efficiency
        + 0.05 * robustness
    )
    return score


def _mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def _sum(values: list[float]) -> float:
    return sum(values) if values else 0.0


def _p95(values: list[float]) -> float:
    if not values:
        return 0.0
    sorted_v = sorted(values)
    idx = int(len(sorted_v) * 0.95)
    return sorted_v[min(idx, len(sorted_v) - 1)]


def main():
    parser = argparse.ArgumentParser(
        description="Autoresearch Dictation & Realtime Performance Harness"
    )
    parser.add_argument(
        "--manifest", default="samples/benchmark/autoresearch/manifest.json"
    )
    parser.add_argument("--binary", default="./voxtral")
    parser.add_argument("--model", default="voxtral-model")
    parser.add_argument(
        "--delays",
        default="480",
        help="Comma-separated delays in ms. For offline files, delay barely "
        "affects inference — use a single representative value (480ms) to "
        "halve eval time. Multi-delay sweeps are for streaming mode.",
    )
    parser.add_argument(
        "--limit", type=int, default=0, help="Max samples per dataset split (0=all)"
    )
    parser.add_argument("--output", default="bench/eval_latest.json")
    parser.add_argument(
        "--warmup",
        action="store_true",
        default=True,
        help="Run one warmup pass before collecting metrics (default: on)",
    )
    parser.add_argument("--no-warmup", dest="warmup", action="store_false")
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    if not manifest_path.exists():
        print(
            f"Error: {manifest_path} not found. Run prepare_datasets.py first.",
            file=sys.stderr,
        )
        return 1

    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)

    if args.limit > 0:
        grouped: dict[str, list] = {}
        for m in manifest:
            grouped.setdefault(m["dataset"], []).append(m)
        manifest = []
        for ds, items in grouped.items():
            manifest.extend(items[: args.limit])

    try:
        delays = [int(d.strip()) for d in args.delays.split(",") if d.strip()]
    except ValueError:
        print("Invalid --delays format.", file=sys.stderr)
        return 1

    commit_sha = get_commit_sha()
    hardware = get_hardware()
    print(f"commit={commit_sha}  hardware={hardware}")

    # Optional warmup: run one file through the binary so GPU shader cache is warm.
    # This prevents the first-run Metal compile spike from inflating that sample's latency.
    if args.warmup and manifest:
        warmup_file = manifest[0]["audio_filepath"]
        print(f"Warmup run on {Path(warmup_file).name} (result discarded)...")
        run_voxtral(args.binary, args.model, warmup_file, delays[0])

    results: list[EvalResult] = []
    total_runs = len(manifest) * len(delays)
    run_idx = 0

    print(
        f"\nEval harness: {len(manifest)} files × {len(delays)} delay(s) = {total_runs} runs"
    )

    for delay in delays:
        print(f"\n--- Delay: {delay} ms ---")
        for item in manifest:
            run_idx += 1
            wav_path = item["audio_filepath"]
            ref_raw = item["reference_text"]
            ds_name = item["dataset"]

            print(
                f"[{run_idx}/{total_runs}] {ds_name} | {Path(wav_path).name}...",
                end="",
                flush=True,
            )

            m = run_voxtral(args.binary, args.model, wav_path, delay)

            if not m["success"]:
                print(f" FAILED ({m['error']})")
                results.append(
                    EvalResult(
                        audio_filepath=wav_path,
                        dataset=ds_name,
                        split=item["split"],
                        delay_ms=delay,
                        reference_text=ref_raw,
                        hypothesis_text="",
                        wer=1.0,
                        cer=1.0,
                        ttft_ms=0.0,
                        ttf_ms=0.0,
                        endpoint_latency_ms=0.0,
                        inference_rtf=0.0,
                        wall_sec=m["wall_sec"],
                        audio_sec=0.0,
                        step_ms=0.0,
                        gpu_mb=0.0,
                        encoder_ms=0.0,
                        decoder_ms=0.0,
                        n_restarts=0,
                        commit_sha=commit_sha,
                        hardware=hardware,
                        success=False,
                        error=m["error"],
                    )
                )
                continue

            # Scoring: dictation datasets keep case+punctuation intact
            is_dictation = "dictation" in ds_name.lower()
            ref_clean = ref_raw.strip() if is_dictation else clean_text(ref_raw)
            hyp_clean = m["stdout"].strip() if is_dictation else clean_text(m["stdout"])

            wer_score = cer_score = 1.0
            if ref_clean:
                try:
                    wer_score = jiwer.wer(ref_clean, hyp_clean)
                    cer_score = jiwer.cer(ref_clean, hyp_clean)
                except ValueError:
                    pass

            print(
                f" WER:{wer_score:.3f} TTFT:{m['ttft_ms']:.0f}ms "
                f"EP:{m['endpoint_latency_ms']:.0f}ms iRTF:{m['inference_rtf']:.3f}"
            )

            results.append(
                EvalResult(
                    audio_filepath=wav_path,
                    dataset=ds_name,
                    split=item["split"],
                    delay_ms=delay,
                    reference_text=ref_clean,
                    hypothesis_text=hyp_clean,
                    wer=wer_score,
                    cer=cer_score,
                    ttft_ms=m["ttft_ms"],
                    ttf_ms=m["ttf_ms"],
                    endpoint_latency_ms=m["endpoint_latency_ms"],
                    inference_rtf=m["inference_rtf"],
                    wall_sec=m["wall_sec"],
                    audio_sec=m["audio_sec"],
                    step_ms=m["step_ms"],
                    gpu_mb=m["gpu_mb"],
                    encoder_ms=m["encoder_ms"],
                    decoder_ms=m["decoder_ms"],
                    n_restarts=m["n_restarts"],
                    commit_sha=commit_sha,
                    hardware=hardware,
                    success=True,
                )
            )

    # ── SCOREBOARD ───────────────────────────────────────────────────────────
    print("\n\n=== SCOREBOARD ===")
    print(f"commit={commit_sha}  hardware={hardware}\n")

    composite_score = compute_composite_score([r for r in results if r.success])
    print(f"  COMPOSITE SCORE: {composite_score:.4f}  (promote if > baseline)\n")

    # Machine-readable METRIC line — grep-able by autoresearch.sh and agents
    # Must appear in stdout so the benchmark script can extract it without JSON parsing
    print(f"METRIC composite_score={composite_score:.4f}")

    # Group by delay then dataset
    by_delay: dict[int, list[EvalResult]] = {}
    for r in results:
        by_delay.setdefault(r.delay_ms, []).append(r)

    for delay, delay_results in sorted(by_delay.items()):
        print(f"Delay {delay}ms:")
        ds_groups: dict[str, list[EvalResult]] = {}
        for r in delay_results:
            ds_groups.setdefault(r.dataset, []).append(r)

        for ds_name, ds_results in ds_groups.items():
            ok = [r for r in ds_results if r.success]
            fail_count = len(ds_results) - len(ok)

            if not ok:
                print(f"  {ds_name}: All {len(ds_results)} runs failed.")
                continue

            # Quality
            wers = [r.wer for r in ok]
            avg_wer = _mean(wers)

            # Latency (inference-only — the signal that matters for optimization)
            ttfts = [r.ttft_ms for r in ok if r.ttft_ms > 0]
            eps = [r.endpoint_latency_ms for r in ok if r.endpoint_latency_ms > 0]
            irtfs = [r.inference_rtf for r in ok if r.inference_rtf > 0]
            steps = [r.step_ms for r in ok if r.step_ms > 0]
            gpus = [r.gpu_mb for r in ok if r.gpu_mb > 0]

            # Application-level (pipeline)
            enc_ms = [r.encoder_ms for r in ok if r.encoder_ms > 0]
            dec_ms = [r.decoder_ms for r in ok if r.decoder_ms > 0]
            restarts = [r.n_restarts for r in ok]

            # Audio duration buckets (useful for understanding short vs long clip behavior)
            short = [r for r in ok if r.audio_sec < 10.0]
            long_ = [r for r in ok if r.audio_sec >= 10.0]

            print(
                f"  {ds_name}  (n={len(ok)}{f', {fail_count} failed' if fail_count else ''}):"
            )
            print(f"    WER:          {avg_wer:.2%}  (p95: {_p95(wers):.2%})")
            print(
                f"    TTFT:         mean {_mean(ttfts):.0f} ms  p95 {_p95(ttfts):.0f} ms"
            )
            print(f"    EP latency:   mean {_mean(eps):.0f} ms  p95 {_p95(eps):.0f} ms")
            print(
                f"    inference RTF: mean {_mean(irtfs):.3f}  p95 {_p95(irtfs):.3f}  (target <0.50)"
            )
            print(f"    step_ms:      {_mean(steps):.2f} ms")
            print(f"    VRAM:         {_mean(gpus):.0f} MB")
            if enc_ms:
                print(
                    f"    pipeline:    enc {_mean(enc_ms):.0f}ms dec {_mean(dec_ms):.0f}ms restarts {_sum(restarts)}"
                )
            if short:
                print(
                    f"    short (<10s): iRTF {_mean([r.inference_rtf for r in short if r.inference_rtf > 0]):.3f}"
                )
            if long_:
                print(
                    f"    long  (>=10s): iRTF {_mean([r.inference_rtf for r in long_ if r.inference_rtf > 0]):.3f}"
                )
        print()

    print(
        "NOTE: inference_rtf = ttf_ms / (audio_sec*1000). Excludes model-load overhead."
    )
    print("      Use this, NOT wall_rtf, to compare optimization candidates.\n")

    # ── SAVE ─────────────────────────────────────────────────────────────────
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "commit_sha": commit_sha,
        "hardware": hardware,
        "composite_score": composite_score,
        "results": [asdict(r) for r in results],
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)

    print(f"Full results saved to {out_path}")

    # Append summary to persistent history — survives context resets and overwritten JSON outputs
    # One JSON line per run; load with: for line in open('bench/autoresearch_history.jsonl'): json.loads(line)
    import datetime

    history_path = out_path.parent / "autoresearch_history.jsonl"
    history_entry = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        "commit_sha": commit_sha,
        "hardware": hardware,
        "mode": "full",
        "n_files": len(manifest),
        "composite_score": round(composite_score, 4),
        "success_rate": round(len([r for r in results if r.success]) / len(results), 3)
        if results
        else 0.0,
        "avg_wer": round(_mean([r.wer for r in results if r.success]), 4)
        if results
        else 1.0,
        "avg_inference_rtf": round(
            _mean(
                [r.inference_rtf for r in results if r.success and r.inference_rtf > 0]
            ),
            4,
        )
        if results
        else 0.0,
    }
    with open(history_path, "a", encoding="utf-8") as hf:
        hf.write(json.dumps(history_entry) + "\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
